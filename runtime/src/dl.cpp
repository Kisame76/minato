#include "dl.h"

#include "APKRuntime/apkrt.h"
#include "guest_call.h"
#include "loader_syscalls.h"
#include "process_exit.h"
#include "signals.h"
#include "vfs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace apkrt::dl {
namespace {

using linker::Object;
using loader::LoadError;

// dlfcn.h on Android arm64.
constexpr int rtld_noload = 0x4;
constexpr int rtld_global = 0x100;
void *const rtld_default = nullptr;
void *const rtld_next = reinterpret_cast<void *>(~std::uintptr_t{0});

constexpr std::uint32_t pt_load = 1;
constexpr unsigned stt_tls = 6;
constexpr std::size_t max_objects = 1024;
constexpr int linux_enoent = 2;

// Where linker64 looks for a library by name after LD_LIBRARY_PATH, for a program in
// /data/local/tmp: the default namespace of linkerconfig's [unrestricted] section (Android 15).
constexpr std::string_view default_library_paths[] = {"/system/lib64", "/system_ext/lib64", "/odm/lib64",
                                                      "/vendor/lib64", "/product/lib64"};

// Android 15 keeps libc.so, libm.so and libdl.so in the runtime APEX and has symlinks to them in
// /system/lib64; linker64 names a library by its real path.
constexpr std::pair<std::string_view, std::string_view> system_links[] = {
    {"/system/lib64/libc.so", "/apex/com.android.runtime/lib64/bionic/libc.so"},
    {"/system/lib64/libm.so", "/apex/com.android.runtime/lib64/bionic/libm.so"},
    {"/system/lib64/libdl.so", "/apex/com.android.runtime/lib64/bionic/libdl.so"},
};

struct Library {
    Object object;
    std::filesystem::path container;  // tells one file from another, as linker64's inode does
    std::vector<Library *> needed;    // its DT_NEEDED, in order
    Library *group_root = nullptr;    // the program, or the library dlopen loaded it with
    bool global = false;              // RTLD_GLOBAL, or loaded with the program
    bool linked = false;              // relocated and listed
};

/// Library containers (<file name>.dylib) the guest finds as the files of a directory.
struct Directory {
    std::string guest;
    std::filesystem::path host;
};

/// A library file the search found: its guest path, real as linker64 reports it, and its container.
struct Found {
    std::string path;
    std::filesystem::path container;
};

struct State {
    std::recursive_mutex mutex;  // linker64's g_dl_mutex is recursive: constructors may dlopen
    std::deque<Library> libraries;  // load order, the program first; a deque keeps handles stable
    std::vector<Directory> directories;
    std::vector<std::string> library_path;  // LD_LIBRARY_PATH as linker64 keeps it
    std::string apex_libraries;             // /apex/<module>/lib64 for a program in /apex/<module>/bin
    std::string program_name;               // argv[0], for linker64's messages
    apkrt_start_main_thread_function start_main_thread = nullptr;
    apkrt_register_tls_module_function register_tls = nullptr;
    void *(*tls_get_addr)(const std::uint64_t *index) = nullptr;
    linker::TlsResolvers tls_resolvers;
    apkrt_loader *loader = nullptr;
    apkrt_object entries[max_objects] = {};  // what dl_iterate_phdr lists
    std::size_t listed = 0;
    int argc = 0;
    char **argv = nullptr;
    char **envp = nullptr;
};

State &state() {
    static auto *instance = new State;  // lives as long as the process, like the objects
    return *instance;
}

// dlerror: one message per thread, as bionic keeps it; the size of bionic's dlerror buffer.
thread_local char error_message[512];
thread_local bool error_pending = false;

void set_error(std::string_view message) {
    std::snprintf(error_message, sizeof error_message, "%.*s", static_cast<int>(message.size()), message.data());
    error_pending = true;
}

std::string_view base_name(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// A loaded library by the name it was asked for, its path or its soname, as linker64's
/// find_loaded_library_by_soname finds one without opening a file.
Library *find_loaded(State &state, std::string_view name) {
    for (auto &library : state.libraries) {
        const auto &object = library.object;
        if (object.name == name || object.path == name || (!object.dynamic.soname.empty() && object.dynamic.soname == name)) {
            return &library;
        }
    }
    return nullptr;
}

/// A loaded library from this file, under whatever name (find_loaded_library_by_inode).
Library *loaded_from(State &state, const std::filesystem::path &container) {
    for (auto &library : state.libraries) {
        if (library.container == container) return &library;
    }
    return nullptr;
}

/// The library file at the guest path `path` (in normal form), if a library directory holds it.
/// A path to where a system link points finds the file through the link.
std::optional<Found> library_at(const State &state, std::string path) {
    std::string real = path;
    for (const auto &[link, target] : system_links) {
        if (path == link) real = target;
        if (path == target) path = link;
    }
    const auto slash = path.rfind('/');
    const auto directory = slash == 0 ? std::string("/") : path.substr(0, slash);
    const auto file = path.substr(slash + 1) + ".dylib";
    for (const auto &entry : state.directories) {
        if (entry.guest == directory && std::filesystem::exists(entry.host / file)) return Found{real, entry.host / file};
    }
    return std::nullopt;
}

/// Looks for a library file as linker64's open_library does: a name with a slash is a path, taken
/// as it is (against the working directory when relative); a plain name is looked for in
/// LD_LIBRARY_PATH, then, for a program of an APEX, in the APEX's libraries, then in the default
/// directories. Every place without the file is an open() that fails on Android and leaves ENOENT in
/// errno; `missed` records that.
std::optional<Found> find_library(const State &state, std::string_view name, bool &missed) {
    const auto look = [&](std::string path) {
        auto found = library_at(state, std::move(path));
        if (!found) missed = true;
        return found;
    };
    if (name.find('/') != std::string_view::npos) return look(vfs::absolute(name, vfs::working_directory()));
    for (const auto &directory : state.library_path) {
        if (auto found = look(std::format("{}/{}", directory, name))) return found;
    }
    // linkerconfig links the namespace of a program in /apex/<module>/bin to its APEX's namespace with all
    // of its libraries, then to the system's (/linkerconfig/com.android.art/ld.config.txt on Android 15).
    // In the loader's one scope that is the APEX's folder first (ADR-0033 decision 7, step b).
    if (!state.apex_libraries.empty()) {
        if (auto found = look(std::format("{}/{}", state.apex_libraries, name))) return found;
    }
    for (const auto directory : default_library_paths) {
        if (auto found = look(std::format("{}/{}", directory, name))) return found;
    }
    return std::nullopt;
}

/// Loads the library file the search found for `name` and appends it to the list.
Library &load_library(State &state, std::string_view name, Found found) {
    const auto image = loader::load(found.container.string());
    auto &library = state.libraries.emplace_back();
    library.object = Object{.name = std::string(base_name(name)), .image = image, .dynamic = linker::read_dynamic(image),
                            .path = std::move(found.path)};
    library.container = std::move(found.container);
    return library;
}

/// Loads every library `root` needs that is not loaded yet, breadth first, as linker64 does, and
/// connects each library to the ones it needs. Returns the new libraries, `root` first. `missed`
/// records a search that did not find a file somewhere.
std::vector<Library *> load_needed(State &state, Library &root, bool program, bool &missed) {
    std::vector<Library *> added{&root};
    for (std::size_t next = 0; next < added.size(); ++next) {
        Library &library = *added[next];
        for (const auto &name : library.object.dynamic.needed) {
            Library *dependency = find_loaded(state, name);
            if (dependency == nullptr) {
                auto found = find_library(state, name, missed);
                if (!found) {
                    if (program && &library == &root) {
                        throw LoadError(std::format("library \"{}\" not found: needed by main executable", name));
                    }
                    throw LoadError(std::format("library \"{}\" not found: needed by {} in namespace (default)", name,
                                                library.object.shown_name()));
                }
                dependency = loaded_from(state, found->container);
                if (dependency == nullptr) {
                    dependency = &load_library(state, name, std::move(*found));
                    added.push_back(dependency);
                }
            }
            library.needed.push_back(dependency);
        }
    }
    return added;
}

/// LD_LIBRARY_PATH from the program's environment as linker64 keeps it (parse_LD_LIBRARY_PATH):
/// the directories that exist, in normal form. A directory exists when it holds libraries or the
/// guest's file system has it.
std::vector<std::string> library_path(const State &state, const char *const envp[]) {
    std::vector<std::string> directories;
    constexpr std::string_view variable = "LD_LIBRARY_PATH=";
    for (; envp != nullptr && *envp != nullptr; ++envp) {
        std::string_view value(*envp);
        if (!value.starts_with(variable)) continue;
        value.remove_prefix(variable.size());
        while (!value.empty()) {
            const auto colon = value.find(':');
            const auto part = value.substr(0, colon);
            value = colon == std::string_view::npos ? std::string_view{} : value.substr(colon + 1);
            if (part.empty()) continue;
            auto directory = vfs::absolute(part, vfs::working_directory());
            bool exists = false;
            for (const auto &entry : state.directories) exists = exists || entry.guest == directory;
            struct stat status;
            if (const auto host = vfs::host_path(directory); host && ::stat(host->c_str(), &status) == 0) {
                exists = exists || S_ISDIR(status.st_mode);
            }
            if (exists) directories.push_back(std::move(directory));
        }
    }
    return directories;
}

/// `root` and the libraries it needs, breadth first: its local group.
std::vector<Library *> local_group(Library &root) {
    std::vector<Library *> group{&root};
    std::set<Library *> seen{&root};
    for (std::size_t next = 0; next < group.size(); ++next) {
        for (auto *dependency : group[next]->needed) {
            if (seen.insert(dependency).second) group.push_back(dependency);
        }
    }
    return group;
}

/// Where the relocations of libraries loaded with `root` look for symbols: the global group (the
/// program, the libraries it needs, RTLD_GLOBAL ones), then root's local group.
std::vector<const Object *> lookup_scope(State &state, Library &root, const std::vector<Library *> &added) {
    std::vector<const Object *> scope;
    std::set<const Object *> seen;
    const std::set<Library *> new_ones(added.begin(), added.end());
    for (auto &library : state.libraries) {
        if (library.global && (library.linked || new_ones.contains(&library)) && seen.insert(&library.object).second) {
            scope.push_back(&library.object);
        }
    }
    for (auto *library : local_group(root)) {
        if (seen.insert(&library->object).second) scope.push_back(&library->object);
    }
    return scope;
}

/// Writes the entry dl_iterate_phdr will list for `library`; listed once `list` counts it.
apkrt_object &entry_for(State &state, Library &library, std::size_t index) {
    if (index >= max_objects) throw LoadError("too many libraries");
    const auto &image = library.object.image;
    auto &entry = state.entries[index];
    entry = apkrt_object{
        .bias = image.bias,
        .phdr = reinterpret_cast<const void *>(image.bias + image.info->phdr),
        .phnum = image.info->phnum,
        .name = library.object.shown_name().c_str(),
        .tls_module_id = 0,
        .tls_offset = 0,
    };
    return entry;
}

void list(State &state, std::size_t count) {
    state.listed = count;
    if (state.loader != nullptr) __atomic_store_n(&state.loader->object_count, count, __ATOMIC_RELEASE);
}

Library *containing(State &state, std::uint64_t address) {
    for (auto &library : state.libraries) {
        if (!library.linked) continue;
        const auto &image = library.object.image;
        const auto *headers = reinterpret_cast<const std::uint32_t *>(image.bias + image.info->phdr);
        for (std::uint32_t index = 0; index < image.info->phnum; ++index) {
            const auto *header = headers + 14 * index;  // Elf64_Phdr: 56 bytes
            std::uint64_t vaddr, size;
            std::memcpy(&vaddr, header + 4, 8);
            std::memcpy(&size, header + 10, 8);
            if (header[0] == pt_load && address >= image.bias + vaddr && address < image.bias + vaddr + size) {
                return &library;
            }
        }
    }
    return nullptr;
}

bool is_handle(State &state, const void *handle) {
    for (auto &library : state.libraries) {
        if (&library == handle && library.linked) return true;
    }
    return false;
}

/// The address dlsym returns for `symbol` of `library`: for a TLS variable, this thread's copy.
void *address_of(State &state, const Library &library, const linker::Symbol &symbol) {
    const auto &object = library.object;
    if ((symbol.info & 0xf) == stt_tls) {
        if (object.static_tls) {
            return static_cast<char *>(apkrt_get_guest_tls()) + object.tls_offset + static_cast<std::int64_t>(symbol.value);
        }
        const std::uint64_t index[2] = {object.tls_module_id, symbol.value};  // TlsIndex
        return call_guest(state.tls_get_addr, index);
    }
    return reinterpret_cast<void *>(linker::symbol_address(object, symbol));
}

// --- libdl, called by guest code through ld-android.so ------------------------------------------
//
// ld-android.so takes a lock of its own before it calls in, as linker64 takes g_dl_mutex, so guest
// threads wait for each other there, in guest code, where signals reach them (ADR-0025, decision 8).
// The runtime's lock only keeps them apart from start-up work, which waits for no guest thread and
// runs guest code under the lock only on the program's thread before it starts any other (the ifunc
// resolvers of load_program, ADR-0028): a plain lock is enough here (decision 9).

void *guest_dlopen(const char *name, int flags, const void *, int *error_number) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    if (name == nullptr) return &state.libraries.front();
    const std::string wanted(name);
    // linker64 searches with open(): one that failed leaves ENOENT in errno, whatever came after.
    struct ReportMissed {
        bool missed = false;
        int *error_number;
        ~ReportMissed() {
            if (missed && error_number != nullptr) *error_number = linux_enoent;
        }
    } report{.error_number = error_number};

    Library *loaded = find_loaded(state, wanted);
    std::optional<Found> found;
    if (loaded == nullptr || !loaded->linked) {
        // Even RTLD_NOLOAD looks for the file: a library is loaded under another name or not at all.
        found = find_library(state, wanted, report.missed);
        if (!found) {
            set_error(std::format("dlopen failed: library \"{}\" not found", wanted));
            return nullptr;
        }
        loaded = loaded_from(state, found->container);
    }
    if (loaded != nullptr && loaded->linked) {
        if (flags & rtld_global) loaded->global = true;
        return loaded;
    }
    if (flags & rtld_noload) {
        set_error(std::format("dlopen failed: library \"{}\" wasn't loaded and RTLD_NOLOAD prevented it", wanted));
        return nullptr;
    }
    const auto before = state.libraries.size();
    try {
        Library *root = &load_library(state, wanted, std::move(*found));
        auto added = load_needed(state, *root, false, report.missed);
        const auto scope = lookup_scope(state, *root, added);
        for (auto *library : added) linker::relocate(library->object, scope);

        // Dynamic TLS for the new ones that have a TLS segment (register_soinfo_tls).
        auto index = state.listed;
        for (auto *library : added) {
            auto &entry = entry_for(state, *library, index++);
            library->object.static_tls = false;
            if (state.register_tls != nullptr) {
                library->object.tls_module_id = call_guest(state.register_tls, &entry, &library->object.tls_generation);
            }
        }
        for (auto *library : added) linker::relocate_tls(library->object, scope, state.tls_resolvers);

        for (auto *library : added) {
            library->linked = true;
            library->group_root = root;
            library->global = (flags & rtld_global) != 0;  // linker64 gives all of them the flags
        }
        list(state, index);
        std::vector<const Object *> objects;
        for (auto *library : added) objects.push_back(&library->object);
        linker::run_constructors(objects, state.argc, state.argv, state.envp, false);
        return root;
    } catch (const std::exception &failure) {
        set_error(std::format("dlopen failed: {}", failure.what()));
        // What dyld loaded stays mapped (ADR-0003); the guest never sees it.
        while (state.libraries.size() > before) state.libraries.pop_back();
        return nullptr;
    }
}

void *guest_dlsym(void *handle, const char *name, const char *version_text, const void *caller) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    if (name == nullptr) {
        set_error("dlsym failed: symbol name is null");
        return nullptr;
    }
    const std::string_view version = version_text != nullptr ? version_text : "";
    const auto shown = version.empty() ? std::string(name) : std::format("{}, version {}", name, version);

    const Library *found_in = nullptr;
    const linker::Symbol *symbol = nullptr;
    const auto search = [&](const Library &library) {
        if (symbol == nullptr && (symbol = linker::find_symbol(library.object, name, version)) != nullptr) {
            found_in = &library;
        }
    };
    if (handle == rtld_default || handle == rtld_next) {
        // All global libraries in load order (after the caller's for RTLD_NEXT), then the caller's
        // local group, as dlsym_linear_lookup does.
        Library *caller_library = containing(state, reinterpret_cast<std::uint64_t>(caller));
        bool started = handle == rtld_default;
        for (auto &library : state.libraries) {
            if (!started) {
                started = &library == caller_library;
                continue;
            }
            if (library.linked && library.global) search(library);
        }
        if (symbol == nullptr && caller_library != nullptr) {
            bool after_caller = handle == rtld_default;
            for (auto *library : local_group(*caller_library->group_root)) {
                if (after_caller) search(*library);
                after_caller = after_caller || library == caller_library;
            }
        }
    } else {
        if (!is_handle(state, handle)) {
            set_error(std::format("dlsym failed: invalid handle: {}", handle));
            return nullptr;
        }
        for (auto *library : local_group(*static_cast<Library *>(handle))) search(*library);
    }
    if (symbol == nullptr) {
        set_error(std::format("undefined symbol: {}", shown));
        return nullptr;
    }
    return address_of(state, *found_in, *symbol);
}

int guest_dlclose(void *handle) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    if (!is_handle(state, handle)) {
        set_error(std::format("dlclose failed: invalid handle: {}", handle));
        return -1;
    }
    return 0;  // loaded code stays loaded (ADR-0003)
}

const char *guest_dlerror() {
    if (!error_pending) return nullptr;
    error_pending = false;
    return error_message;
}

/// Dl_info on Android.
struct DlInfo {
    const char *fname;
    void *fbase;
    const char *sname;
    void *saddr;
};

int guest_dladdr(const void *address, void *raw_info) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    const auto target = reinterpret_cast<std::uint64_t>(address);
    const Library *library = containing(state, target);
    if (library == nullptr) return 0;
    auto &info = *static_cast<DlInfo *>(raw_info);
    const auto &object = library->object;
    info = {object.shown_name().c_str(), reinterpret_cast<void *>(object.image.bias), nullptr, nullptr};
    if (const auto *symbol = linker::find_symbol_by_address(object, target)) {
        info.sname = linker::symbol_name(object, *symbol).data();  // NUL-terminated in the string table
        info.saddr = reinterpret_cast<void *>(object.image.bias + symbol->value);
    }
    return 1;
}

/// Ends the process the way linker64 ends a program it cannot link: the message on stderr, then in
/// the log, which is stderr here too, in logcat's tag format as our liblog.so writes it.
[[noreturn]] void fail(const std::string &message) {
    const auto text = std::format("CANNOT LINK EXECUTABLE \"{}\": {}", state().program_name, message);
    const auto lines = std::format("{}\nF/{:<8}: {}\n", text, "linker", text);
    static_cast<void>(::write(STDERR_FILENO, lines.data(), lines.size()));
    process::end(EXIT_FAILURE);
}

}  // namespace

void set_loader_functions(apkrt_loader &loader) {
    loader.dlopen = apkrt_loader_dlopen;
    loader.dlsym = apkrt_loader_dlsym;
    loader.dlclose = apkrt_loader_dlclose;
    loader.dlerror = apkrt_loader_dlerror;
    loader.dladdr = apkrt_loader_dladdr;
}

long loader_syscall(const apkrt_syscall_frame &frame) {
    const auto *x = frame.x;
    switch (x[8]) {
        case APKRT_NR_DLOPEN:
            return reinterpret_cast<long>(guest_dlopen(reinterpret_cast<const char *>(x[0]), static_cast<int>(x[1]),
                                                       reinterpret_cast<const void *>(x[2]), reinterpret_cast<int *>(x[3])));
        case APKRT_NR_DLSYM:
            return reinterpret_cast<long>(guest_dlsym(reinterpret_cast<void *>(x[0]), reinterpret_cast<const char *>(x[1]),
                                                      reinterpret_cast<const char *>(x[2]), reinterpret_cast<const void *>(x[3])));
        case APKRT_NR_DLCLOSE: return guest_dlclose(reinterpret_cast<void *>(x[0]));
        case APKRT_NR_DLERROR: return reinterpret_cast<long>(guest_dlerror());
        case APKRT_NR_DLADDR:
            return guest_dladdr(reinterpret_cast<const void *>(x[0]), reinterpret_cast<void *>(x[1]));
        default: return -38;  // ENOSYS: not one of the stubs' numbers
    }
}

std::recursive_mutex &lock() {
    return state().mutex;
}

void add_library_directory(std::string_view guest_directory, const std::filesystem::path &host_directory) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    state.directories.push_back({vfs::normalize(guest_directory), host_directory});
}

void load_program(const std::filesystem::path &container, const loader::Image &image, const char *const argv[],
                  const char *const envp[]) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    state.program_name = argv != nullptr && argv[0] != nullptr ? argv[0] : "";
    // On the program's thread (ADR-0028), with nobody to return to: whatever keeps the program from
    // linking ends it, the reading of its own dynamic section and of bionic's symbols included.
    try {
        const auto path = vfs::absolute(state.program_name, vfs::working_directory());
        // The containers next to the program's are the library files in its directory.
        const auto slash = path.rfind('/');
        state.directories.push_back({slash == 0 ? std::string("/") : path.substr(0, slash), container.parent_path()});
        state.library_path = library_path(state, envp);
        if (const auto bin = path.find("/bin/", 6); path.starts_with("/apex/") && bin != std::string::npos &&
                                                    path.find('/', 6) == bin) {
            state.apex_libraries = path.substr(0, bin) + "/lib64";
        }

        auto &program = state.libraries.emplace_back();
        program.object = Object{.name = container.stem().string(), .image = image,
                                .dynamic = linker::read_dynamic(image), .path = path};
        program.container = container;
        bool missed = false;  // libc.so's start sets errno to 0 anyway
        const auto added = load_needed(state, program, true, missed);
        for (auto *library : added) {
            library->global = true;
            library->group_root = &program;
        }
        const auto scope = lookup_scope(state, program, added);
        for (auto *library : added) linker::relocate(library->object, scope);
        for (std::size_t index = 0; index < added.size(); ++index) entry_for(state, *added[index], index);
        state.listed = added.size();

        // bionic's parts: libc.so's start function, TLS resolvers and registration, and the table in
        // ld-android.so through which libdl reaches this file.
        for (auto *library : added) {
            const auto &object = library->object;
            const auto at = [&](const linker::Symbol *symbol) { return object.image.bias + symbol->value; };
            if (const auto *symbol = linker::find_symbol(object, APKRT_START_MAIN_THREAD, "APKRT_PRIVATE")) {
                state.start_main_thread = reinterpret_cast<apkrt_start_main_thread_function>(at(symbol));
            }
            if (const auto *symbol = linker::find_symbol(object, APKRT_REGISTER_TLS_MODULE, "APKRT_PRIVATE")) {
                state.register_tls = reinterpret_cast<apkrt_register_tls_module_function>(at(symbol));
            }
            if (const auto *symbol = linker::find_symbol(object, APKRT_SIGRETURN, "APKRT_PRIVATE")) {
                signals::set_default_restorer(at(symbol));
            }
            if (const auto *symbol = linker::find_symbol(object, "__tls_get_addr", "LIBC_R")) {
                state.tls_get_addr = reinterpret_cast<void *(*)(const std::uint64_t *)>(at(symbol));
            }
            if (const auto *symbol = linker::find_symbol(object, APKRT_TLSDESC_RESOLVERS, "APKRT_PRIVATE")) {
                const auto *resolvers = reinterpret_cast<const std::uint64_t *>(at(symbol));
                state.tls_resolvers = {.static_resolver = resolvers[APKRT_TLSDESC_STATIC],
                                       .dynamic_resolver = resolvers[APKRT_TLSDESC_DYNAMIC],
                                       .unresolved_weak_resolver = resolvers[APKRT_TLSDESC_UNRESOLVED_WEAK]};
            }
            if (const auto *symbol = linker::find_symbol(object, APKRT_LOADER, "APKRT_PRIVATE")) {
                auto &loader = *reinterpret_cast<apkrt_loader *>(at(symbol));
                loader.objects = state.entries;
                set_loader_functions(loader);
                state.loader = &loader;
            }
        }
    } catch (const std::exception &failure) {
        fail(failure.what());
    }
}

apkrt_start_main_thread_function start_main_thread() {
    return state().start_main_thread;
}

apkrt_start guest_start(void *start_stack) {
    auto &state = ::apkrt::dl::state();
    return apkrt_start{start_stack, state.entries, state.listed};
}

std::vector<MappedObject> mapped_objects() {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    std::vector<MappedObject> objects;
    for (const auto &library : state.libraries) {
        const auto &image = library.object.image;
        const auto &info = *image.info;
        objects.push_back({.path = library.object.shown_name(),
                           .container = library.container,
                           .bias = image.bias,
                           .image_begin = image.bias + info.min_vaddr,
                           .image_end = image.bias + info.min_vaddr + info.image_size,
                           .program_headers = info.phdr != 0 ? reinterpret_cast<const void *>(image.bias + info.phdr)
                                                             : nullptr,
                           .program_header_count = info.phdr != 0 ? info.phnum : 0});
    }
    return objects;
}

void finish_start(int argc, char **argv, char **envp) {
    auto &state = ::apkrt::dl::state();
    std::vector<const Object *> objects;
    {
        const std::lock_guard lock(state.mutex);
        state.argc = argc;
        state.argv = argv;
        state.envp = envp;
        std::size_t index = 0;
        for (auto &library : state.libraries) {
            library.object.tls_module_id = state.entries[index].tls_module_id;
            library.object.tls_offset = state.entries[index].tls_offset;
            objects.push_back(&library.object);
            ++index;
        }
        try {
            for (const auto *object : objects) linker::relocate_tls(*object, objects, state.tls_resolvers);
        } catch (const std::exception &failure) {
            fail(failure.what());
        }
        for (auto &library : state.libraries) library.linked = true;
        list(state, state.listed);
    }
    linker::run_constructors(objects, argc, argv, envp, true);
}

}  // namespace apkrt::dl
