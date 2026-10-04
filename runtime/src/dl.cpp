#include "dl.h"

#include "APKRuntime/apkrt.h"
#include "guest_call.h"
#include "loader_syscalls.h"
#include "process_exit.h"
#include "signals.h"
#include "vfs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

// The libraries that linkerconfig's [system] section links from the default namespace, that of a program
// outside the APEXes, to com_android_art and com_android_i18n (/linkerconfig/ld.config.txt on Android 15). They
// resolve in ART's folder, after the default directories. ICU's libraries live with ART's there, in
// com_android_art, not in com_android_i18n: ADR-0040 decision 2's departure.
constexpr std::string_view art_libraries = "/apex/com.android.art/lib64";
constexpr std::string_view linked_to_art[] = {"libdexfile.so", "libjdwp.so", "libnativebridge.so",
                                              "libnativehelper.so", "libnativeloader.so", "libsigchain.so",
                                              // com_android_i18n's on Android
                                              "libandroidicu.so", "libicu.so", "libicui18n.so", "libicuuc.so"};

// The JNI libraries of an APEX's own jars, which linkerconfig takes from the APEX's manifest into
// /linkerconfig/apex.libraries.config.txt: Android 15's for the namespaces Minato has, without com_android_art's
// libartservice.so, which Minato does not ship.
constexpr std::pair<std::string_view, std::string_view> apex_jni_libraries[] = {
    {"com_android_conscrypt", "libjavacrypto.so"}};

// What com_android_art's libraries take from the default namespace: bionic's and libz.so, which Android's ART
// APEX holds itself and Minato's guest system has in /system/lib64 (ADR-0040 decision 2; the departure for
// libz.so). And what Conscrypt's libjavacrypto.so takes from it.
constexpr std::string_view art_needs[] = {"libc.so", "libm.so", "libdl.so", "libdl_android.so", "liblog.so",
                                          "libz.so"};
constexpr std::string_view conscrypt_needs[] = {"libc.so", "libm.so", "libdl.so", "liblog.so"};

// The exempt list of linker.cpp at the tag (is_exempt_lib): private system libraries that an app below target SDK
// 24 may still load, with a warning.
constexpr std::string_view exempt_libraries[] = {"libandroid_runtime.so", "libbinder.so", "libcrypto.so",
                                                 "libcutils.so", "libexpat.so", "libgui.so", "libmedia.so",
                                                 "libnativehelper.so", "libssl.so", "libstagefright.so",
                                                 "libsqlite.so", "libui.so", "libutils.so"};
constexpr std::string_view bionic_changes =
    "https://android.googlesource.com/platform/bionic/+/main/android-changes-for-ndk-developers.md";

// Android 15 keeps libc.so, libm.so and libdl.so in the runtime APEX and has symlinks to them in
// /system/lib64; linker64 names a library by its real path.
constexpr std::pair<std::string_view, std::string_view> system_links[] = {
    {"/system/lib64/libc.so", "/apex/com.android.runtime/lib64/bionic/libc.so"},
    {"/system/lib64/libm.so", "/apex/com.android.runtime/lib64/bionic/libm.so"},
    {"/system/lib64/libdl.so", "/apex/com.android.runtime/lib64/bionic/libdl.so"},
};

struct Namespace;

struct Library {
    Object object;
    std::filesystem::path container;  // tells one file from another, as linker64's inode does
    std::vector<Library *> needed;    // its DT_NEEDED, in order
    Library *group_root = nullptr;    // the program, or the library dlopen loaded it with
    bool global = false;              // RTLD_GLOBAL, or loaded with the program
    bool linked = false;              // relocated and listed
    Namespace *ns = nullptr;          // the namespace it was loaded into, its primary one (ADR-0040)
    int target_sdk = 10000;           // the app's target SDK version when dlopen loaded it (target_sdk_version_)
};

/// A link of a namespace to another, for some sonames or all (android_namespace_link_t).
struct NamespaceLink {
    Namespace *to;
    std::set<std::string> sonames;
    bool all = false;

    bool accessible(std::string_view soname) const {
        return all || sonames.contains(std::string(soname));
    }
};

/// A linker namespace as linker64 keeps one (android_namespace_t, ADR-0040 decision 1): where it looks for a
/// library by name, where an isolated one may load a file besides, the libraries it holds, and its links.
struct Namespace {
    std::string name;
    bool isolated = false;
    bool exempt_list = false;  // ANDROID_NAMESPACE_TYPE_EXEMPT_LIST_ENABLED
    std::vector<std::string> ld_library_paths;
    std::vector<std::string> default_library_paths;
    std::vector<std::string> permitted_paths;
    std::vector<Library *> libraries;  // its soinfo list, in load order
    std::vector<NamespaceLink> links;
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
    // The namespaces, default, com_android_art and com_android_conscrypt first (ADR-0040 decision 2), then those
    // that libnativeloader creates; a deque keeps the handles stable.
    std::deque<Namespace> namespaces;
    // The anonymous namespace, once a namespace asked to be it (ANDROID_NAMESPACE_TYPE_ALSO_USED_AS_ANONYMOUS); the
    // default one until then, as linker64's g_anonymous_namespace.
    Namespace *anonymous = nullptr;
    std::vector<Directory> directories;
    std::vector<std::string> library_path;  // LD_LIBRARY_PATH as linker64 keeps it
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

/// The namespaces at the start (ADR-0040 decision 2), as linkerconfig's [system] section has them for a program
/// outside the APEXes on Android 15: default with LD_LIBRARY_PATH and the system's folders, linked to
/// com_android_art for linked_to_art's sonames; com_android_art and com_android_conscrypt isolated, each with its
/// APEX's lib64, linked to default for what their libraries need of the system's.
void make_namespaces(State &state) {
    state.namespaces.clear();
    state.anonymous = nullptr;
    auto &system = state.namespaces.emplace_back(Namespace{.name = "default", .ld_library_paths = state.library_path});
    for (const auto folder : default_library_paths) system.default_library_paths.emplace_back(folder);
    auto &art = state.namespaces.emplace_back(Namespace{.name = "com_android_art", .isolated = true,
                                                        .default_library_paths = {std::string(art_libraries)}});
    auto &conscrypt = state.namespaces.emplace_back(Namespace{
        .name = "com_android_conscrypt", .isolated = true, .default_library_paths = {"/apex/com.android.conscrypt/lib64"}});
    system.links.push_back({.to = &art, .sonames = {std::begin(linked_to_art), std::end(linked_to_art)}});
    art.links.push_back({.to = &system, .sonames = {std::begin(art_needs), std::end(art_needs)}});
    conscrypt.links.push_back({.to = &system, .sonames = {std::begin(conscrypt_needs), std::end(conscrypt_needs)}});
}

/// The namespace a program starts in: an APEX's for a program in its bin, as linkerconfig gives a program of an APEX
/// its APEX's namespace on Android 15, else default. Another APEX than ART's and Conscrypt's gets one on its lib64,
/// named as libnativeloader names an APEX's, linked to default for every library.
Namespace &namespace_of_program(State &state, std::string_view path) {
    if (path.starts_with("/apex/com.android.art/bin/")) return state.namespaces[1];
    if (path.starts_with("/apex/com.android.conscrypt/bin/")) return state.namespaces[2];
    const auto module_end = path.starts_with("/apex/") ? path.find('/', 6) : std::string_view::npos;
    if (module_end == std::string_view::npos || !path.substr(module_end).starts_with("/bin/")) {
        return state.namespaces[0];
    }
    std::string name(path.substr(6, module_end - 6));
    std::ranges::replace(name, '.', '_');
    auto &space = state.namespaces.emplace_back(Namespace{
        .name = name, .isolated = true, .default_library_paths = {std::string(path.substr(0, module_end)) + "/lib64"}});
    space.links.push_back({.to = &state.namespaces[0], .all = true});
    return space;
}

/// A namespace's name as linker64's messages show it: the default one as "(default)".
std::string shown_name(const Namespace &space) {
    return space.name == "default" ? "(default)" : space.name;
}

/// Whether `library` answers to `name`: the name it was asked for or its soname, as linker64 compares sonames.
bool answers_to(const Library &library, std::string_view name) {
    const auto &object = library.object;
    return object.name == name || (!object.dynamic.soname.empty() && object.dynamic.soname == name);
}

/// A loaded library that `space` holds and that answers to `name`, as find_loaded_library_by_soname finds one
/// without opening a file, and with `through_links` one that a linked namespace holds, through a link that gives
/// the name. A name with a slash is a path, which a soname never is.
Library *loaded_in(Namespace &space, std::string_view name, bool through_links) {
    if (name.find('/') != std::string_view::npos) return nullptr;
    for (auto *library : space.libraries) {
        if (answers_to(*library, name)) return library;
    }
    if (!through_links) return nullptr;
    for (const auto &link : space.links) {
        if (!link.accessible(name)) continue;
        for (auto *library : link.to->libraries) {
            if (answers_to(*library, name)) return library;
        }
    }
    return nullptr;
}

/// A loaded library from this file, under whatever name and in whatever namespace: one file has one instance here
/// (ADR-0040 decision 6).
Library *loaded_from(State &state, const std::filesystem::path &container) {
    for (auto &library : state.libraries) {
        if (library.container == container) return &library;
    }
    return nullptr;
}

/// A loaded library from this file that `space` holds, and with `through_links` one that a linked namespace holds,
/// through a link that gives its soname, as linker64's find_loaded_library_by_inode finds one.
Library *loaded_from(Namespace &space, const std::filesystem::path &container, bool through_links) {
    for (auto *library : space.libraries) {
        if (library->container == container) return library;
    }
    if (!through_links) return nullptr;
    for (const auto &link : space.links) {
        for (auto *library : link.to->libraries) {
            const auto &soname = library->object.dynamic.soname;
            if (library->container == container && link.accessible(soname.empty() ? library->object.name : soname)) {
                return library;
            }
        }
    }
    return nullptr;
}

/// Whether `file` lies in `folder` itself (linker64's file_is_in_dir), or with `below` anywhere under it
/// (file_is_under_dir).
bool in_folder(std::string_view file, std::string_view folder, bool below) {
    if (!file.starts_with(folder) || file.size() <= folder.size() || file[folder.size()] != '/') return false;
    return below || file.find('/', folder.size() + 1) == std::string_view::npos;
}

/// Whether the library file at the real path `path` may load into `space`, as android_namespace_t::is_accessible
/// decides: into a namespace that is not isolated any file; into an isolated one a file in one of its folders, or
/// anywhere under one of its permitted paths.
bool accessible(const Namespace &space, std::string_view path) {
    if (!space.isolated) return true;
    for (const auto *folders : {&space.ld_library_paths, &space.default_library_paths}) {
        for (const auto &folder : *folders) {
            if (in_folder(path, folder, false)) return true;
        }
    }
    return std::ranges::any_of(space.permitted_paths, [&](const auto &folder) { return in_folder(path, folder, true); });
}

/// The app's target SDK version, which android_set_application_target_sdk_version keeps in ld-android.so's table;
/// __ANDROID_API_FUTURE__ until it is set.
int target_sdk_version(const State &state) {
    return state.loader != nullptr ? state.loader->target_sdk_version : 10000;
}

/// Whether the real path `path` is a system library: one in a folder of the default namespace (is_system_library).
bool system_library(const State &state, std::string_view path) {
    return std::ranges::any_of(state.namespaces[0].default_library_paths,
                               [&](const auto &folder) { return in_folder(path, folder, false); });
}

/// Whether a link of `space` gives `name`, a path by its file name (maybe_accessible_via_namespace_links).
bool linked_for(const Namespace &space, std::string_view name) {
    return std::ranges::any_of(space.links, [&](const NamespaceLink &link) { return link.accessible(base_name(name)); });
}

/// Whether the exempt list lets `space` load `name` (is_exempt_lib): below target SDK 24, a library that a system
/// library needs unless a link gives it, or one of the list, by its name or by its path in /system/lib64.
bool exempt(const State &state, const Namespace &space, std::string_view name, const Library *needed_by) {
    if (target_sdk_version(state) >= 24) return false;
    if (needed_by != nullptr && system_library(state, needed_by->object.shown_name())) return !linked_for(space, name);
    if (name.starts_with('/') && name.substr(0, name.rfind('/')) == "/system/lib64") name = base_name(name);
    return std::ranges::find(exempt_libraries, name) != std::end(exempt_libraries);
}

/// A line of linker64's log, with the tag "linker", in logcat's tag format as our liblog.so writes it to stderr.
void log_line(char priority, std::string_view text) {
    const auto line = std::format("{}/{:<8}: {}\n", priority, "linker", text);
    static_cast<void>(::write(STDERR_FILENO, line.data(), line.size()));
}

/// linker64's DL_WARN: the warning in the log, then on stderr.
void warn(std::string_view text) {
    log_line('W', text);
    const auto line = std::format("WARNING: linker: {}\n", text);
    static_cast<void>(::write(STDERR_FILENO, line.data(), line.size()));
}

/// Folders separated by colons, as linker64 joins them for its messages.
std::string joined(const std::vector<std::string> &folders) {
    std::string text;
    for (const auto &folder : folders) text += (text.empty() ? "" : ":") + folder;
    return text;
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

/// A library file on `space`'s folders, LD_LIBRARY_PATH's part first, as linker64's open_library_on_paths looks.
/// Every folder without the file is an open() that fails on Android and leaves ENOENT in errno; `missed` records it.
std::optional<Found> found_on_paths(const State &state, const Namespace &space, std::string_view name, bool &missed) {
    for (const auto *paths : {&space.ld_library_paths, &space.default_library_paths}) {
        for (const auto &directory : *paths) {
            if (auto found = library_at(state, std::format("{}/{}", directory, name))) return found;
            missed = true;
        }
    }
    return std::nullopt;
}

/// Where the search found a library: loaded already, or a file and the namespace to load it into; or why a file it
/// found may not load.
struct Located {
    Library *loaded = nullptr;
    std::optional<Found> found;
    Namespace *space = nullptr;
    std::string refused;
};

/// The file at the path `name` for `space`, as linker64's load_library takes a file it opened: an instance from it
/// that `space` holds or reaches through a link; else the file, when `space` may load it or the exempt list lets it,
/// with linker64's warning; else linker64's refusal, and the file through a link that gives its name, into the
/// linked namespace (find_library_in_linked_namespace). `requester` needs the library or calls dlopen for it.
Located locate_path(State &state, Namespace &space, std::string_view name, const Library *requester, bool needed,
                    bool &missed) {
    auto found = library_at(state, vfs::absolute(name, vfs::working_directory()));
    if (!found) {
        missed = true;
        return {};
    }
    if (auto *loaded = loaded_from(space, found->container, true)) return {.loaded = loaded};
    if (accessible(space, found->path)) return {.found = std::move(found), .space = &space};
    const Library *needed_by = needed ? requester : nullptr;
    const auto by = requester != nullptr ? requester->object.shown_name() : std::string("(unknown)");
    if (exempt(state, space, name, needed_by)) {
        if (needed_by == nullptr || !system_library(state, needed_by->object.shown_name())) {
            warn(std::format("Warning: library \"{}\" (\"{}\") needed or dlopened by \"{}\" is not accessible by "
                             "namespace \"{}\" and will not work when the app moves to API level 24 or later ({}#{}) "
                             "(allowing for now because this app's target API level is still {})",
                             name, found->path, by, shown_name(space), bionic_changes,
                             "private-api-enforced-for-api-level-24", target_sdk_version(state)));
        }
        return {.found = std::move(found), .space = &space};
    }
    Located refused{.refused = std::format("library \"{}\" needed or dlopened by \"{}\" is not accessible for the "
                                           "namespace \"{}\"", name, by, shown_name(space))};
    if (!linked_for(space, name)) {
        log_line('E', std::format("library \"{}\" (\"{}\") needed or dlopened by \"{}\" is not accessible for the "
                                  "namespace: [name=\"{}\", ld_library_paths=\"{}\", default_library_paths=\"{}\", "
                                  "permitted_paths=\"{}\"]",
                                  name, found->path, by, shown_name(space), joined(space.ld_library_paths),
                                  joined(space.default_library_paths), joined(space.permitted_paths)));
    }
    for (const auto &link : space.links) {
        if (!link.accessible(base_name(name))) continue;
        if (auto *loaded = loaded_from(*link.to, found->container, false)) return {.loaded = loaded};
        if (accessible(*link.to, found->path)) return {.found = std::move(found), .space = link.to};
    }
    return refused;
}

/// Looks for a library as linker64's find_library_internal does in `space` (ADR-0040 decision 1). A name with a
/// slash is a path, taken as it is (against the working directory when relative), as locate_path takes it. A plain
/// name is a loaded library that `space` holds, or a linked namespace through a link that gives the name; else a
/// file on `space`'s folders; below target SDK 24 in a namespace with the exempt list one of the list on the default
/// namespace's folders, whose links then count instead; else one on a linked namespace's folders, through such a
/// link, which it loads into. `requester` needs the library (`needed`) or calls dlopen for it.
Located locate(State &state, Namespace &space, std::string_view name, const Library *requester, bool needed,
               bool &missed) {
    if (name.find('/') != std::string_view::npos) return locate_path(state, space, name, requester, needed, missed);
    if (auto *loaded = loaded_in(space, name, true)) return {.loaded = loaded};
    if (auto found = found_on_paths(state, space, name, missed)) return {.found = std::move(found), .space = &space};
    // find_library_internal asks the exempt list with the requester as needed_by, dlopen's caller included.
    Namespace *from = &space;
    if (space.exempt_list && exempt(state, space, name, requester)) {
        from = &state.namespaces[0];
        if (auto found = found_on_paths(state, *from, name, missed)) return {.found = std::move(found), .space = from};
    }
    for (const auto &link : from->links) {
        if (!link.accessible(name)) continue;
        if (auto found = found_on_paths(state, *link.to, name, missed)) {
            return {.found = std::move(found), .space = link.to};
        }
    }
    return {};
}

/// Puts `library` into `space`'s list, unless it is there: as linker64's add_soinfo.
void add_to(Namespace &space, Library &library) {
    if (std::ranges::find(space.libraries, &library) == space.libraries.end()) space.libraries.push_back(&library);
}

/// Puts an instance of another namespace into `space`'s list, where linker64 would load the file again: one file
/// has one instance here (ADR-0040 decision 6). The log says so once for each namespace that takes it.
void take(Namespace &space, Library &library) {
    if (std::ranges::find(space.libraries, &library) != space.libraries.end()) return;
    if (library.ns != nullptr && library.ns != &space) {
        log_line('I', std::format("namespace \"{}\" takes library \"{}\" from namespace \"{}\", where linker64 would "
                                  "load the file again (ADR-0040 decision 6)",
                                  shown_name(space), library.object.shown_name(), shown_name(*library.ns)));
    }
    space.libraries.push_back(&library);
}

/// The namespace of a caller in `caller`, else the anonymous one, as linker64's get_caller_namespace.
Namespace &caller_namespace(State &state, const Library *caller) {
    if (caller != nullptr && caller->ns != nullptr) return *caller->ns;
    return state.anonymous != nullptr ? *state.anonymous : state.namespaces[0];
}

/// Loads the library file the search found for `name` into `space` and appends it to the list. One marked
/// DF_1_GLOBAL joins every namespace, as linker64 links such a library to all of them.
Library &load_library(State &state, std::string_view name, Found found, Namespace &space) {
    const auto image = loader::load(found.container.string());
    auto &library = state.libraries.emplace_back();
    library.object = Object{.name = std::string(base_name(name)), .image = image, .dynamic = linker::read_dynamic(image),
                            .path = std::move(found.path)};
    library.container = std::move(found.container);
    library.ns = &space;
    add_to(space, library);
    if (library.object.dynamic.global) {
        for (auto &each : state.namespaces) add_to(each, library);
    }
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
            // In the namespace of the library that needs it, as linker64 looks for and loads it.
            auto located = locate(state, *library.ns, name, &library, true, missed);
            Library *dependency = located.loaded;
            if (dependency == nullptr) {
                if (!located.refused.empty()) throw LoadError(located.refused);
                if (!located.found) {
                    if (program && &library == &root) {
                        throw LoadError(std::format("library \"{}\" not found: needed by main executable", name));
                    }
                    throw LoadError(std::format("library \"{}\" not found: needed by {} in namespace {}", name,
                                                library.object.shown_name(), shown_name(*library.ns)));
                }
                dependency = loaded_from(state, located.found->container);
                if (dependency == nullptr) {
                    dependency = &load_library(state, name, std::move(*located.found), *located.space);
                    added.push_back(dependency);
                } else {
                    take(*located.space, *dependency);
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

/// Where the relocations of libraries loaded with `root` look for symbols, as linker64's soinfo_do_lookup
/// does: the global group, which is only the program and libraries marked DF_1_GLOBAL, as linked with -z
/// global (android_namespace_t::get_global_group, ADR-0040 decision 1), then root's local group. Not every
/// RTLD_GLOBAL library is in it: a library opened with dlopen finds its own definitions before libc.so's.
std::vector<const Object *> lookup_scope(State &state, Library &root, const std::vector<Library *> &added) {
    std::vector<const Object *> scope;
    std::set<const Object *> seen;
    const std::set<Library *> new_ones(added.begin(), added.end());
    for (auto &library : state.libraries) {
        const bool in_global_group = &library == &state.libraries.front() || library.object.dynamic.global;
        if (in_global_group && (library.linked || new_ones.contains(&library)) &&
            seen.insert(&library.object).second) {
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

/// The folders of a namespace's search path, separated by colons, as linker64 keeps them (parse_path).
std::vector<std::string> split_search_path(std::string_view value) {
    std::vector<std::string> folders;
    while (!value.empty()) {
        const auto colon = value.find(':');
        const auto part = value.substr(0, colon);
        value = colon == std::string_view::npos ? std::string_view{} : value.substr(colon + 1);
        if (!part.empty()) folders.push_back(vfs::absolute(part, vfs::working_directory()));
    }
    return folders;
}

/// Adds `parent`'s shared group to `space`, as linker64's create_namespace does for a namespace that is not
/// shared (get_shared_group): for the default namespace its global group (the program and DF_1_GLOBAL libraries),
/// for another one its RTLD_GLOBAL libraries.
void add_shared_group(State &state, Namespace &space, const Namespace &parent) {
    const bool is_default = &parent == &state.namespaces[0];
    for (auto *library : parent.libraries) {
        const bool global_group = library == &state.libraries.front() || library->object.dynamic.global;
        if (is_default ? global_group : library->global) add_to(space, *library);
    }
}

/// The namespace of a search path that an ld-android.so of ADR-0033 e0 hands over (dlopen_in): the path's folders,
/// then everything a plain dlopen finds, as e0 had it. So a shared copy of the default namespace, with its
/// libraries and its links to com_android_art, behind those folders.
Namespace &search_path_namespace(State &state, std::string_view search_path) {
    auto folders = split_search_path(search_path);
    const std::string name = "(search path " + std::string(search_path) + ")";
    for (auto &space : state.namespaces) {
        if (space.name == name) return space;
    }
    const Namespace &system = state.namespaces[0];
    auto &space = state.namespaces.emplace_back(Namespace{.name = name, .ld_library_paths = std::move(folders)});
    space.ld_library_paths.insert(space.ld_library_paths.end(), system.ld_library_paths.begin(),
                                  system.ld_library_paths.end());
    space.default_library_paths = system.default_library_paths;
    space.links = system.links;
    for (auto *library : system.libraries) add_to(space, *library);
    return space;
}

/// The namespace `handle` stands for, or null when it is none of the loader's.
Namespace *as_namespace(State &state, const void *handle) {
    for (auto &space : state.namespaces) {
        if (&space == handle) return &space;
    }
    return nullptr;
}

/// dlopen, or android_dlopen_ext in the namespace `in`, or in the one of an e0 ld-android.so's `search_path`.
void *guest_dlopen(const char *name, int flags, const void *caller, int *error_number,
                   const char *search_path = nullptr, Namespace *in = nullptr) {
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

    // The namespace it opens in: the one android_dlopen_ext names, else the caller's, as linker64 takes the
    // caller's (get_caller_namespace); a caller outside every library gets the anonymous one.
    const Library *calling = containing(state, reinterpret_cast<std::uint64_t>(caller));
    Namespace &space = in != nullptr            ? *in
                       : search_path != nullptr ? search_path_namespace(state, search_path)
                                                : caller_namespace(state, calling);
    // Even RTLD_NOLOAD looks for the file: a library is loaded under another name or not at all.
    auto located = locate(state, space, wanted, calling, false, report.missed);
    Library *loaded = located.loaded;
    if (loaded == nullptr) {
        if (!located.found) {
            set_error(located.refused.empty() ? std::format("dlopen failed: library \"{}\" not found", wanted)
                                              : "dlopen failed: " + located.refused);
            return nullptr;
        }
        loaded = loaded_from(state, located.found->container);
        if (loaded != nullptr) take(*located.space, *loaded);
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
        Library *root = &load_library(state, wanted, std::move(*located.found), *located.space);
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
            // linker64 gives all of them the flags, and DF_1_GLOBAL implies RTLD_GLOBAL (soinfo::set_dt_flags_1).
            library->global = (flags & rtld_global) != 0 || library->object.dynamic.global;
            // The local group root's target SDK version, which soinfo::get_target_sdk_version gives each of them.
            library->target_sdk = target_sdk_version(state);
        }
        list(state, index);
        std::vector<const Object *> objects;
        for (auto *library : added) objects.push_back(&library->object);
        linker::run_constructors(objects, state.argc, state.argv, state.envp, false);
        return root;
    } catch (const std::exception &failure) {
        set_error(std::format("dlopen failed: {}", failure.what()));
        // What dyld loaded stays mapped (ADR-0003); the guest never sees it, nor does any namespace.
        std::set<const Library *> dropped;
        for (auto index = before; index < state.libraries.size(); ++index) dropped.insert(&state.libraries[index]);
        for (auto &each : state.namespaces) {
            std::erase_if(each.libraries, [&](const Library *library) { return dropped.contains(library); });
        }
        while (state.libraries.size() > before) state.libraries.pop_back();
        return nullptr;
    }
}

// android_create_namespace's types (linker.h at the tag).
constexpr std::uint64_t namespace_isolated = 1;
constexpr std::uint64_t namespace_shared = 2;
constexpr std::uint64_t namespace_exempt_list = 0x08000000;
constexpr std::uint64_t namespace_also_used_as_anonymous = 0x10000000;

/// android_create_namespace, as linker64's create_namespace does (ADR-0040 decision 4): the parent is the given
/// one, else the caller's namespace; a shared namespace takes the parent's folders, permitted paths, libraries and
/// links, another one the parent's shared group. One that asks to be the anonymous namespace becomes it, unless one
/// is already (set_anonymous_namespace).
void *guest_create_namespace(const char *name, const char *ld_library_path, const char *default_library_path,
                             std::uint64_t type, const char *permitted_path, void *parent_handle, const void *caller) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    Namespace *parent = parent_handle != nullptr ? as_namespace(state, parent_handle) : nullptr;
    if (parent_handle != nullptr && parent == nullptr) {
        set_error("android_create_namespace failed: invalid parent namespace");
        return nullptr;
    }
    if (parent == nullptr) {
        parent = &caller_namespace(state, containing(state, reinterpret_cast<std::uint64_t>(caller)));
    }
    auto &space = state.namespaces.emplace_back(Namespace{
        .name = name != nullptr ? name : "", .isolated = (type & namespace_isolated) != 0,
        .exempt_list = (type & namespace_exempt_list) != 0,
        .ld_library_paths = split_search_path(ld_library_path != nullptr ? ld_library_path : ""),
        .default_library_paths = split_search_path(default_library_path != nullptr ? default_library_path : ""),
        .permitted_paths = split_search_path(permitted_path != nullptr ? permitted_path : "")});
    if ((type & namespace_shared) != 0) {
        space.ld_library_paths.insert(space.ld_library_paths.end(), parent->ld_library_paths.begin(),
                                      parent->ld_library_paths.end());
        space.default_library_paths.insert(space.default_library_paths.end(), parent->default_library_paths.begin(),
                                           parent->default_library_paths.end());
        space.permitted_paths.insert(space.permitted_paths.end(), parent->permitted_paths.begin(),
                                     parent->permitted_paths.end());
        for (auto *library : parent->libraries) add_to(space, *library);
        space.links = parent->links;
    } else {
        add_shared_group(state, space, *parent);
    }
    if ((type & namespace_also_used_as_anonymous) != 0) {
        if (state.anonymous != nullptr) {
            set_error(std::format("android_create_namespace failed: failed to set namespace: [name=\"{}\", "
                                  "ld_library_path=\"{}\", default_library_paths=\"{}\" permitted_paths=\"{}\"] as the "
                                  "anonymous namespace",
                                  space.name, joined(space.ld_library_paths), joined(space.default_library_paths),
                                  joined(space.permitted_paths)));
            state.namespaces.pop_back();
            return nullptr;
        }
        state.anonymous = &space;
    }
    return &space;
}

/// android_link_namespaces and android_link_namespaces_all_libs (`sonames` null): links `from` to `to` for the
/// sonames, separated by colons, or for all.
int guest_link_namespaces(void *from_handle, void *to_handle, const char *sonames) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    Namespace *from = as_namespace(state, from_handle);
    Namespace *to = as_namespace(state, to_handle);
    if (from == nullptr || to == nullptr) {
        set_error("android_link_namespaces failed: invalid namespace");
        return -1;
    }
    NamespaceLink link{.to = to, .all = sonames == nullptr};
    if (sonames != nullptr) {
        if (*sonames == '\0') {
            set_error("android_link_namespaces failed: the list of shared libraries is empty");
            return -1;
        }
        for (std::string_view rest(sonames); !rest.empty();) {
            const auto colon = rest.find(':');
            if (const auto part = rest.substr(0, colon); !part.empty()) link.sonames.emplace(part);
            rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        }
    }
    from->links.push_back(std::move(link));
    return 0;
}

/// android_get_exported_namespace: "default" for the program's namespace, as for a program in /system/bin
/// (ADR-0040 decision 2), com_android_art and com_android_conscrypt for the APEXes'; no "system".
void *guest_exported_namespace(const char *name) {
    auto &state = ::apkrt::dl::state();
    const std::lock_guard lock(state.mutex);
    if (name == nullptr || state.namespaces.size() < 3) return nullptr;
    for (std::size_t index = 0; index < 3; ++index) {
        if (state.namespaces[index].name == name) return &state.namespaces[index];
    }
    return nullptr;
}

/// android_dlopen_ext with ANDROID_DLEXT_USE_NAMESPACE.
void *guest_dlopen_namespace(const char *name, int flags, const void *caller, int *error_number, void *handle) {
    Namespace *space = nullptr;
    {
        auto &state = ::apkrt::dl::state();
        const std::lock_guard lock(state.mutex);
        space = as_namespace(state, handle);
    }
    if (space == nullptr) {
        set_error("android_dlopen_ext failed: invalid namespace");
        return nullptr;
    }
    return guest_dlopen(name, flags, caller, error_number, nullptr, space);
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
        // The libraries of the caller's namespace in its order (after the caller for RTLD_NEXT), those opened with
        // RTLD_GLOBAL or by an app below target SDK 23, then the caller's local group, as dlsym_linear_lookup does.
        Library *caller_library = containing(state, reinterpret_cast<std::uint64_t>(caller));
        bool started = handle == rtld_default;
        for (auto *library : caller_namespace(state, caller_library).libraries) {
            if (!started) {
                started = library == caller_library;
                continue;
            }
            if (library->linked && (library->global || library->target_sdk < 23)) search(*library);
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

void set_loader_functions(apkrt_loader &loader, std::size_t size) {
    loader.dlopen = apkrt_loader_dlopen;
    loader.dlsym = apkrt_loader_dlsym;
    loader.dlclose = apkrt_loader_dlclose;
    loader.dlerror = apkrt_loader_dlerror;
    loader.dladdr = apkrt_loader_dladdr;
    // ADR-0033 e0's and e2a's, which an older ld-android.so's table ends before.
    if (size >= offsetof(apkrt_loader, dlopen_in) + sizeof loader.dlopen_in) loader.dlopen_in = apkrt_loader_dlopen_in;
    if (size >= offsetof(apkrt_loader, dlopen_namespace) + sizeof loader.dlopen_namespace) {
        loader.create_namespace = apkrt_loader_create_namespace;
        loader.link_namespaces = apkrt_loader_link_namespaces;
        loader.exported_namespace = apkrt_loader_exported_namespace;
        loader.dlopen_namespace = apkrt_loader_dlopen_namespace;
    }
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
        case APKRT_NR_DLOPEN_IN:
            return reinterpret_cast<long>(guest_dlopen(reinterpret_cast<const char *>(x[0]), static_cast<int>(x[1]),
                                                       reinterpret_cast<const void *>(x[2]), reinterpret_cast<int *>(x[3]),
                                                       reinterpret_cast<const char *>(x[4])));
        case APKRT_NR_CREATE_NAMESPACE:
            return reinterpret_cast<long>(guest_create_namespace(
                reinterpret_cast<const char *>(x[0]), reinterpret_cast<const char *>(x[1]),
                reinterpret_cast<const char *>(x[2]), x[3], reinterpret_cast<const char *>(x[4]),
                reinterpret_cast<void *>(x[5]), reinterpret_cast<const void *>(x[6])));
        case APKRT_NR_LINK_NAMESPACES:
            return guest_link_namespaces(reinterpret_cast<void *>(x[0]), reinterpret_cast<void *>(x[1]),
                                         reinterpret_cast<const char *>(x[2]));
        case APKRT_NR_EXPORTED_NAMESPACE:
            return reinterpret_cast<long>(guest_exported_namespace(reinterpret_cast<const char *>(x[0])));
        case APKRT_NR_DLOPEN_NAMESPACE:
            return reinterpret_cast<long>(guest_dlopen_namespace(
                reinterpret_cast<const char *>(x[0]), static_cast<int>(x[1]), reinterpret_cast<const void *>(x[2]),
                reinterpret_cast<int *>(x[3]), reinterpret_cast<void *>(x[4])));
        default: return -38;  // ENOSYS: not one of the stubs' numbers
    }
}

std::recursive_mutex &lock() {
    return state().mutex;
}

std::string apex_libraries_config() {
    // linkerconfig gives an APEX the libraries of public.libraries.txt it provides; com_android_art provides those
    // the default namespace links to it.
    std::set<std::string, std::less<>> public_libraries;
    if (const auto host = vfs::host_path("/system/etc/public.libraries.txt")) {
        if (FILE *file = std::fopen(host->c_str(), "r")) {
            char line[256];
            while (std::fgets(line, sizeof line, file) != nullptr) {
                const std::string_view text(line);
                const auto name = text.substr(0, text.find_first_of(" \t\r\n#"));
                if (!name.empty()) public_libraries.emplace(name);
            }
            std::fclose(file);
        }
    }
    std::string art_public;
    for (const auto name : linked_to_art) {
        if (!public_libraries.contains(name)) continue;
        if (!art_public.empty()) art_public += ':';
        art_public += name;
    }
    // One line for each, by the namespace's name, as linkerconfig sorts them; libnativeloader keeps the last line
    // of a namespace and tag.
    std::string text;
    if (!art_public.empty()) text += std::format("public com_android_art {}\n", art_public);
    for (const auto &[space, libraries] : apex_jni_libraries) text += std::format("jni {} {}\n", space, libraries);
    return text;
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
        make_namespaces(state);

        auto &program = state.libraries.emplace_back();
        program.object = Object{.name = container.stem().string(), .image = image,
                                .dynamic = linker::read_dynamic(image), .path = path};
        program.container = container;
        // In its namespace, and in every other one, as linker64 adds somain to all of them.
        program.ns = &namespace_of_program(state, path);
        for (auto &space : state.namespaces) add_to(space, program);
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
                set_loader_functions(loader, symbol->size);
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
