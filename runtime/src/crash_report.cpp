#include "crash_report.h"

#include <cstring>

#include <mach/mach.h>

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
