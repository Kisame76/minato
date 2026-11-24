#include "crash_report.h"

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstring>

#include <fcntl.h>
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
std::atomic<const std::uint64_t *> heap_window_base_at{nullptr};
std::atomic<const std::uint64_t *> heap_window_size_at{nullptr};
}  // namespace

void note_heap_window(const std::uint64_t *base, const std::uint64_t *size) {
    heap_window_size_at.store(size, std::memory_order_release);
    heap_window_base_at.store(base, std::memory_order_release);
}

void heap_window(std::uint64_t &base, std::uint64_t &size) {
    const std::uint64_t *base_at = heap_window_base_at.load(std::memory_order_acquire);
    const std::uint64_t *size_at = heap_window_size_at.load(std::memory_order_acquire);
    // ART sets the two once, before any object exists; read them as the guest left them.
    base = base_at != nullptr ? *static_cast<const volatile std::uint64_t *>(base_at) : 0;
    size = size_at != nullptr && base != 0 ? *static_cast<const volatile std::uint64_t *>(size_at) : 0;
}

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

namespace {

/// A load or store as the fault's form names it.
struct Access {
    const char *kind = "load";  // "load", "store" or "prefetch"
    const char *size = "";      // "byte", "half", "word", "double", "simd", "pair-word", "pair-double", "pair-simd"
    const char *mode = "";      // the addressing mode
    int base = -1;              // Rn (31: sp); -1 for a literal, whose base is the pc
    int index = -1;             // Rm of a register offset (31: xzr)
    std::int64_t imm = 0;       // the offset the address adds (0 for a post-index, which adds it afterwards)
    unsigned shift = 0;         // a register offset's shift
    unsigned extend = 3;        // a register offset's option: 2 UXTW, 3 LSL, 6 SXTW, 7 SXTX
    bool computable = false;    // whether base (+ index) + imm is the address the access starts at
    bool has_imm = false;       // whether imm is part of the address (not for a post-index or a register offset)
};

constexpr const char *access_sizes[] = {"byte", "half", "word", "double"};

std::int64_t sign_extend(std::uint64_t value, unsigned bits) {
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

/// A general load or store's kind from its opc (load/store register forms, not SIMD).
const char *general_kind(unsigned size, unsigned opc) {
    if (opc == 0) return "store";
    return size == 3 && opc == 2 ? "prefetch" : "load";
}

/// Decodes the A64 loads and stores whose address a register gives (ARM ARM C4.1.66 and C4.1.94 classes); false for
/// any other word.
bool decode_access(std::uint32_t w, Access &a) {
    const unsigned size = w >> 30;
    const bool simd = ((w >> 26) & 1u) != 0;
    const auto rn = static_cast<int>((w >> 5) & 31u);
    const unsigned opc = (w >> 22) & 3u;
    const unsigned scale = simd && (opc & 2u) != 0 ? 4u : size;
    if ((w & 0x3B00'0000u) == 0x3900'0000u) {  // register, unsigned immediate
        a.kind = simd ? ((opc & 1u) != 0 ? "load" : "store") : general_kind(size, opc);
        a.size = simd ? "simd" : access_sizes[size];
        a.mode = "unsigned-offset";
        a.base = rn;
        a.imm = static_cast<std::int64_t>(std::uint64_t{(w >> 10) & 0xfffu} << scale);
        a.computable = true;
        a.has_imm = true;
        return true;
    }
    if ((w & 0x3B20'0000u) == 0x3800'0000u) {  // register, 9-bit signed immediate
        constexpr const char *modes[] = {"unscaled", "post-index", "unprivileged", "pre-index"};
        const unsigned type = (w >> 10) & 3u;
        a.kind = simd ? ((opc & 1u) != 0 ? "load" : "store") : general_kind(size, opc);
        a.size = simd ? "simd" : access_sizes[size];
        a.mode = modes[type];
        a.base = rn;
        a.imm = type == 1 ? 0 : sign_extend((w >> 12) & 0x1ffu, 9);
        a.computable = true;
        a.has_imm = type != 1;
        return true;
    }
    if ((w & 0x3B20'0C00u) == 0x3820'0800u) {  // register, register offset
        a.kind = simd ? ((opc & 1u) != 0 ? "load" : "store") : general_kind(size, opc);
        a.size = simd ? "simd" : access_sizes[size];
        a.mode = "register-offset";
        a.base = rn;
        a.index = static_cast<int>((w >> 16) & 31u);
        a.extend = (w >> 13) & 7u;
        a.shift = ((w >> 12) & 1u) != 0 ? scale : 0;
        a.computable = true;
        return true;
    }
    if ((w & 0x3B20'0C00u) == 0x3820'0000u && !simd) {  // atomic memory operation (LDADD, SWP, ...)
        a.kind = "atomic";
        a.size = access_sizes[size];
        a.mode = "atomic";
        a.base = rn;
        a.computable = true;
        return true;
    }
    if ((w & 0x3A00'0000u) == 0x2800'0000u) {  // pair
        constexpr const char *modes[] = {"pair-no-allocate", "pair-post-index", "pair-offset", "pair-pre-index"};
        const unsigned type = (w >> 23) & 3u;
        const unsigned pair_scale = simd ? 2u + size : 2u + (size >> 1);
        a.kind = ((w >> 22) & 1u) != 0 ? "load" : "store";
        a.size = simd ? "pair-simd" : (size >> 1) != 0 ? "pair-double" : "pair-word";
        a.mode = modes[type];
        a.base = rn;
        a.imm = type == 1 ? 0 : sign_extend((w >> 15) & 0x7fu, 7) * (std::int64_t{1} << pair_scale);
        a.computable = true;
        a.has_imm = type != 1;
        return true;
    }
    if ((w & 0x3B00'0000u) == 0x1800'0000u) {  // literal: pc-relative, no base register
        a.kind = (w >> 30) == 3 && !simd ? "prefetch" : "load";
        a.size = simd ? "simd" : (w >> 30) == 1 ? "double" : "word";
        a.mode = "literal";
        a.imm = sign_extend((w >> 5) & 0x7ffffu, 19) * 4;
        a.has_imm = true;  // from the pc
        return true;
    }
    if ((w & 0x3F00'0000u) == 0x0800'0000u) {  // exclusive or ordered (LDXR, LDAR, STLR, CAS, ...)
        a.kind = ((w >> 22) & 1u) != 0 ? "load" : "store";
        a.size = access_sizes[size];
        a.mode = "exclusive";
        a.base = rn;
        a.computable = true;
        return true;
    }
    if ((w & 0xBE00'0000u) == 0x0C00'0000u) {  // SIMD structures (LD1..LD4, ST1..ST4)
        a.kind = ((w >> 22) & 1u) != 0 ? "load" : "store";
        a.size = "simd";
        a.mode = "simd-structure";
        a.base = rn;
        a.computable = true;
        return true;
    }
    return false;
}

void append_register(char *out, std::size_t cap, std::size_t &at, int number, const char *thirty_one) {
    if (number == 31) {
        append(out, cap, at, thirty_one);
        return;
    }
    append(out, cap, at, "x");
    append_decimal(out, cap, at, static_cast<unsigned long>(number));
}

/// The registers among x0..x30 that hold `value`, as "x1,x18", or "-" for none.
void append_registers_holding(char *out, std::size_t cap, std::size_t &at, const std::uint64_t (&registers)[31],
                              std::uint64_t value) {
    bool any = false;
    for (int number = 0; number < 31; ++number) {
        if (registers[number] != value) continue;
        if (any) append(out, cap, at, ",");
        append_register(out, cap, at, number, "");
        any = true;
    }
    if (!any) append(out, cap, at, "-");
}

/// The faulting word's form: kind, size, addressing mode, base and index registers with their values. `has_base` and
/// `base_value`: whether it reads through a base register, and that register's value (sp's for sp).
void append_form(char *out, std::size_t cap, std::size_t &at, const FaultSite &site, bool &has_base,
                 std::uint64_t &base_value) {
    has_base = false;
    if (!site.instruction_read) {
        append(out, cap, at, "unreadable");
        return;
    }
    Access a;
    if (!decode_access(site.instruction, a)) {
        append(out, cap, at, "none");
        return;
    }
    append(out, cap, at, a.kind);
    append(out, cap, at, " ");
    append(out, cap, at, a.size);
    append(out, cap, at, " ");
    append(out, cap, at, a.mode);
    if (a.base >= 0) {
        append(out, cap, at, " base ");
        append_register(out, cap, at, a.base, "sp");
        has_base = true;
        base_value = a.base == 31 ? site.sp : site.registers[a.base];
        append(out, cap, at, "=0x");
        append_hex(out, cap, at, base_value);
    }
    if (a.has_imm) {
        append(out, cap, at, a.imm < 0 ? " imm -0x" : " imm 0x");
        append_hex(out, cap, at, a.imm < 0 ? 0 - static_cast<std::uint64_t>(a.imm) : static_cast<std::uint64_t>(a.imm));
    }
    if (a.index >= 0) {
        append(out, cap, at, " index ");
        append_register(out, cap, at, a.index, "xzr");
        if (a.index != 31) {
            append(out, cap, at, "=0x");
            append_hex(out, cap, at, site.registers[a.index]);
        }
        if (a.extend != 3 || a.shift != 0) {  // anything but a plain 64-bit index: its extend, then its shift
            constexpr const char *extends[] = {"uxtb", "uxth", "uxtw", "lsl", "sxtb", "sxth", "sxtw", "sxtx"};
            append(out, cap, at, " ");
            append(out, cap, at, extends[a.extend & 7u]);
            if (a.shift != 0) {
                append(out, cap, at, " ");
                append_decimal(out, cap, at, a.shift);
            }
        }
    }
}

/// What the base's value is among the small numbers Minato hands a guest where Android hands an address (FaultSite,
/// crash_report.h's base-is): every class that matches, comma-separated. fcntl(F_GETFD) is async-signal-safe.
void append_base_class(char *out, std::size_t cap, std::size_t &at, const FaultSite &site, bool has_base,
                       std::uint64_t value) {
    if (!has_base) {
        append(out, cap, at, "-");
        return;
    }
    if (value == 0) {
        append(out, cap, at, "null");
        return;
    }
    constexpr std::uint64_t four_gib = std::uint64_t{1} << 32;
    const std::uint64_t pid = site.pid > 0 ? static_cast<std::uint64_t>(site.pid) : 0;
    const std::uint64_t last_tid = site.last_tid > 0 ? static_cast<std::uint64_t>(site.last_tid) : 0;
    const bool classes[] = {
        site.heap_window_size != 0 && value < site.heap_window_size && (value & 7u) == 0,  // an ART reference
        pid != 0 && value == pid,
        pid != 0 && value > pid && value <= last_tid,
        value <= INT_MAX && ::fcntl(static_cast<int>(value), F_GETFD) != -1,
        value < four_gib && (value & 3u) == 2,  // ART's kGlobal
        value < four_gib && (value & 3u) == 3,  // kWeakGlobal
        value >= four_gib && (value & 3u) == 1,  // kLocal: an entry's address with the kind in its low bits
    };
    constexpr const char *names[] = {"heap-ref", "pid", "tid", "fd", "jni-global", "jni-weak", "jni-local"};
    bool any = false;
    for (std::size_t index = 0; index < std::size(names); ++index) {
        if (!classes[index]) continue;
        if (any) append(out, cap, at, ",");
        append(out, cap, at, names[index]);
        any = true;
    }
    if (!any) append(out, cap, at, "none");
}

// The engines' and runtimes' own libraries among the allow list (keep_libs), by role: everything else is "other".
constexpr string_view engine_libs[] = {"SDL2", "SDL3", "cocos2d", "flutter", "fmod", "gdx", "godot_android", "hermes",
                                       "jsc", "main", "mono", "monosgen-2.0", "openal", "python", "renpython", "unity",
                                       "v8"};

/// The library's name without "lib" and ".so", as kept_soname compares it.
string_view bare_name(string_view soname) {
    if (starts_with(soname, "lib")) soname.remove_prefix(3);
    if (soname.size() > 3 && soname.substr(soname.size() - 3) == ".so") soname.remove_suffix(3);
    return soname;
}

void append_offset(char *out, std::size_t cap, std::size_t &at, std::uint64_t address, const Module &module) {
    append(out, cap, at, "+0x");
    append_hex(out, cap, at, address - module.base);
}

}  // namespace

const char *library_role(string_view soname) {
    const string_view name = bare_name(soname);
    if (name == "il2cpp" || starts_with(name, "il2cpp_") || starts_with(name, "il2cpp-")) return "IL2CPP";
    if (!kept_soname(soname)) return "other";
    for (const string_view engine : engine_libs) {
        if (name == engine) return "engine";
        if (name.size() > engine.size() && starts_with(name, engine)) {
            const char next = name[engine.size()];
            if (next == '_' || next == '-' || next == '.') return "engine";
        }
    }
    for (const string_view prefix : {string_view("cocos2d"), string_view("fmod"), string_view("gdx"),
                                      string_view("mono"), string_view("python"), string_view("v8")}) {
        if (starts_with(name, prefix)) return "engine";
    }
    return "other";
}

void write_fault_site_line(int fd, const FaultSite &site) {
    const Module module = resolve_registered(site.pc, nullptr);
    if (!module.found) return;
    char line[1536];  // the longest: 31 registers in both lists, every form field and 32 register values
    std::size_t at = 0;
    constexpr std::size_t cap = sizeof line;
    append(line, cap, at, "E/apkrt: fault-site ");
    const char *name = fault_signal_name(site.signal);
    append(line, cap, at, name != nullptr ? name : "SIG?");
    append(line, cap, at, " lib ");
    append(line, cap, at, library_role(module.soname));
    append(line, cap, at, " pc ");
    append_offset(line, cap, at, site.pc, module);
    append(line, cap, at, " lr ");
    const Module caller = resolve_registered(site.lr, nullptr);
    if (caller.found && caller.base == module.base) {
        append_offset(line, cap, at, site.lr, caller);
    } else if (caller.found) {
        append(line, cap, at, library_role(caller.soname));
        append(line, cap, at, " ");
        append_offset(line, cap, at, site.lr, caller);
    } else {
        append(line, cap, at, "0x");
        append_hex(line, cap, at, site.lr);
    }
    append(line, cap, at, " far 0x");
    append_hex(line, cap, at, site.far);
    append(line, cap, at, " x18 0x");
    append_hex(line, cap, at, site.registers[18]);
    append(line, cap, at, " gx18 0x");
    append_hex(line, cap, at, site.guest_x18);
    append(line, cap, at, site.guest_x18_renamed ? " rename" : " slot11");
    append(line, cap, at, " zero ");
    append_registers_holding(line, cap, at, site.registers, 0);
    append(line, cap, at, " far-in ");
    append_registers_holding(line, cap, at, site.registers, site.far);
    append(line, cap, at, " rewritten-words ");
    if (site.rewritten_words < 0) {
        append(line, cap, at, "unknown");
    } else {
        append_decimal(line, cap, at, static_cast<unsigned long>(site.rewritten_words));
    }
    append(line, cap, at, " form ");
    bool has_base = false;
    std::uint64_t base_value = 0;
    append_form(line, cap, at, site, has_base, base_value);
    append(line, cap, at, " base-is ");
    append_base_class(line, cap, at, site, has_base, base_value);
    append(line, cap, at, " heap-window ");
    if (site.heap_window_base != 0) {
        append(line, cap, at, "0x");
        append_hex(line, cap, at, site.heap_window_base);
    } else {
        append(line, cap, at, "-");
    }
    append(line, cap, at, " va=");
    append_decimal(line, cap, at, static_cast<unsigned long>(site.virtual_size >> 20));
    append(line, cap, at, " vahole=");
    append_decimal(line, cap, at, static_cast<unsigned long>(site.largest_hole >> 20));
    append(line, cap, at, " regs");
    for (std::size_t number = 0; number < std::size(site.registers); ++number) {
        append(line, cap, at, " x");
        append_decimal(line, cap, at, number);
        append(line, cap, at, "=0x");
        append_hex(line, cap, at, site.registers[number]);
    }
    append(line, cap, at, " sp=0x");
    append_hex(line, cap, at, site.sp);
    append(line, cap, at, "\n");
    static_cast<void>(::write(fd, line, at));
}

}  // namespace apkrt::crash
