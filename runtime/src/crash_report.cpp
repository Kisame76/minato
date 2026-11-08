#include "crash_report.h"

#include <atomic>
#include <cstddef>
#include <cstring>

#include <mach/mach.h>
#include <unistd.h>

namespace apkrt::crash {
namespace {

using std::string_view;

// Engine, Android and Minato libraries whose name may appear in a crash record; any other library is written
// as "app library N" (privacy). Sorted by byte value, unique, basenames only: no "lib" prefix, no ".so".
constexpr string_view keep_libs[] = {
    "EGL", "GLESv1_CM", "GLESv2", "GLESv3", "OpenSLES", "SDL2", "SDL3",
    "aaudio", "android", "android_runtime", "apkrt", "app", "art",
    "c", "c++", "c++_shared", "camera2ndk", "cocos2d", "crashlytics", "crypto",
    "dl", "flutter", "fmod", "gdx", "godot_android", "hermes", "hwui",
    "icu", "il2cpp", "jnigraphics", "jsc", "ld-android", "linker64", "log",
    "m", "main", "mediandk", "mono", "monosgen-2.0", "nativeloader", "nativewindow",
    "openal", "python", "renpython", "sqlite", "ssl", "unity", "v8", "vulkan", "z"};

// Of those, the names kept by a bare prefix: a soname that merely starts with one (e.g. "python3.11",
// "icuuc", "v8android", "fmodstudio") is kept. The others match only exactly or followed by '_' '-' or '.'.
constexpr string_view prefix_libs[] = {"cocos2d", "crashlytics", "fmod", "gdx", "icu", "mono", "python", "v8"};

bool starts_with(string_view s, string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

bool kept_name(string_view name) {
    for (const string_view k : keep_libs) {
        if (name == k) return true;
        if (name.size() > k.size() && starts_with(name, k)) {
            const char c = name[k.size()];
            if (c == '_' || c == '-' || c == '.') return true;
        }
    }
    for (const string_view k : prefix_libs)
        if (starts_with(name, k)) return true;
    return false;
}

// Async-signal-safe buffer appends: no allocation, bounded by `cap`.
void append(char *out, std::size_t cap, std::size_t &at, string_view text) {
    for (char c : text)
        if (at < cap) out[at++] = c;
}

void append_decimal(char *out, std::size_t cap, std::size_t &at, unsigned long value) {
    char digits[20];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0 && at < cap) out[at++] = digits[--count];
}

void append_hex(char *out, std::size_t cap, std::size_t &at, std::uint64_t value) {
    char digits[16];
    int count = 0;
    do {
        digits[count++] = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    } while (value != 0);
    while (count > 0 && at < cap) out[at++] = digits[--count];
}

}  // namespace

bool kept_soname(string_view soname) {
    string_view name = soname;
    if (starts_with(name, "lib")) name.remove_prefix(3);
    if (const auto so = name.find(".so"); so != string_view::npos) name = name.substr(0, so);
    return kept_name(name);
}

std::span<const string_view> keep_libs_list() { return keep_libs; }

void format_frame(char *out, std::size_t cap, std::size_t &at, std::uint64_t address, const Module &module) {
    if (!module.found) {
        append(out, cap, at, "0x");
        append_hex(out, cap, at, address);
        return;
    }
    if (kept_soname(module.soname)) {
        append(out, cap, at, module.soname);
    } else {
        append(out, cap, at, "app library ");
        append_decimal(out, cap, at, static_cast<unsigned long>(module.app_index));
    }
    append(out, cap, at, "+0x");
    append_hex(out, cap, at, address - module.base);
}

Module resolve_in_objects(std::uint64_t address, std::span<const LoadedObject> objects) {
    int app = 0;  // counts the non-listed objects in load order, for "app library N"
    for (const LoadedObject &object : objects) {
        const bool kept = kept_soname(object.soname);
        if (!kept) ++app;
        if (address >= object.image_begin && address < object.image_end)
            return {.found = true, .base = object.bias, .soname = object.soname, .app_index = kept ? 0 : app};
    }
    return {.found = false};
}

namespace {

// The append-only registry a signal handler reads without a lock or allocation (the signal-frames path). Modules are
// never unloaded (ADR-0003), so the loader only appends: it writes an entry, then releases the count; a reader
// acquires the count and iterates [0, count). The loader serializes loads under its own lock, so there is one writer.
struct RegisteredModule {
    std::uint64_t image_begin = 0;
    std::uint64_t image_end = 0;
    std::uint64_t bias = 0;
    char soname[64] = {};  // a bounded copy: the loader's name is a view into a transient buffer (loader.cpp realpath)
    std::size_t soname_size = 0;
};
constexpr std::size_t max_modules = 512;
RegisteredModule registered_modules[max_modules];
std::atomic<std::size_t> registered_count{0};

}  // namespace

void register_module(std::uint64_t image_begin, std::uint64_t image_end, std::uint64_t bias, string_view soname) {
    const std::size_t at = registered_count.load(std::memory_order_relaxed);  // one writer: the loader, under its lock
    if (at >= max_modules) return;  // full: the signal path shows these addresses bare, never a wrong name
    RegisteredModule &module = registered_modules[at];
    module.image_begin = image_begin;
    module.image_end = image_end;
    module.bias = bias;
    const std::size_t size = soname.size() < sizeof module.soname ? soname.size() : sizeof module.soname - 1;
    std::memcpy(module.soname, soname.data(), size);  // copied, so the caller's view need not outlive the call
    module.soname[size] = '\0';
    module.soname_size = size;
    registered_count.store(at + 1, std::memory_order_release);  // the entry is complete before the count is seen
}

Module resolve_registered(std::uint64_t address, void *) {
    const std::size_t count = registered_count.load(std::memory_order_acquire);
    int app = 0;  // as resolve_in_objects: the non-listed modules in load order
    for (std::size_t index = 0; index < count; ++index) {
        const RegisteredModule &module = registered_modules[index];
        const string_view soname(module.soname, module.soname_size);
        const bool kept = kept_soname(soname);
        if (!kept) ++app;
        if (address >= module.image_begin && address < module.image_end)
            return {.found = true, .base = module.bias, .soname = soname, .app_index = kept ? 0 : app};
    }
    return {.found = false};
}

void reset_modules() { registered_count.store(0, std::memory_order_release); }

namespace {

/// The name for a Linux signal number the diagnostic reports, or null for one it does not (then "SIG<n>").
const char *fault_signal_name(int signal) {
    switch (signal) {
        case 4: return "SIGILL";
        case 5: return "SIGTRAP";
        case 6: return "SIGABRT";
        case 7: return "SIGBUS";
        case 8: return "SIGFPE";
        case 11: return "SIGSEGV";
        default: return nullptr;
    }
}

}  // namespace

void write_fault_line(int fd, int signal, int code, std::uint64_t fault_address, std::uint64_t pc) {
    char line[256];  // the longest line (a 63-char soname, a negative code, two 16-digit addresses) fits with room
    std::size_t at = 0;
    append(line, sizeof line, at, "E/apkrt: fault ");
    if (const char *name = fault_signal_name(signal)) {
        append(line, sizeof line, at, name);
    } else {
        append(line, sizeof line, at, "SIG");
        append_decimal(line, sizeof line, at, static_cast<unsigned long>(signal));
    }
    append(line, sizeof line, at, " code ");
    if (code < 0) {  // a raised signal's si_code (SI_QUEUE, SI_TKILL) is negative; a fault's is positive
        append(line, sizeof line, at, "-");
        append_decimal(line, sizeof line, at, static_cast<unsigned long>(-static_cast<long>(code)));
    } else {
        append_decimal(line, sizeof line, at, static_cast<unsigned long>(code));
    }
    append(line, sizeof line, at, " at ");
    format_frame(line, sizeof line, at, pc, resolve_registered(pc, nullptr));
    append(line, sizeof line, at, ", addr 0x");
    append_hex(line, sizeof line, at, fault_address);
    append(line, sizeof line, at, ", to the app's handler\n");
    static_cast<void>(::write(fd, line, at));
}

int write_backtrace(char *out, std::size_t cap, std::size_t &at, std::uint64_t fp, int limit,
                    Module (*resolve)(std::uint64_t, void *), void *ctx) {
    int written = 0;
    std::uint64_t frame = fp;
    for (int depth = 0; depth < limit && frame != 0 && frame % 8 == 0; ++depth) {
        // The guest's frame record {caller's frame, return address}, read without a fault: a chain may lead to
        // a page the process cannot read (a guard page, a thread's stack end), where the backtrace ends.
        std::uint64_t record[2] = {0, 0};
        vm_size_t read = 0;  // vm_read_overwrite: iOS's SDK has no mach_vm.h
        if (::vm_read_overwrite(::mach_task_self(), static_cast<vm_address_t>(frame), sizeof record,
                                reinterpret_cast<vm_address_t>(record), &read) != KERN_SUCCESS ||
            read != sizeof record) {
            break;
        }
        const std::uint64_t return_address = record[1];
        if (return_address == 0) break;
        append(out, cap, at, "#");
        append_decimal(out, cap, at, static_cast<unsigned long>(written));
        append(out, cap, at, " ");
        format_frame(out, cap, at, return_address, resolve(return_address, ctx));
        append(out, cap, at, "\n");
        ++written;
        // The chain goes up the stack: a caller's frame lies above, and not far (a broken chain ends here).
        const std::uint64_t caller = record[0];
        if (caller <= frame || caller - frame > (1u << 20)) break;
        frame = caller;
    }
    return written;
}

}  // namespace apkrt::crash
