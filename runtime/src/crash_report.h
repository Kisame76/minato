// A crash names its cause (ADR-0032 decision 8): at a guest's fatal signal the runtime writes a bounded,
// async-signal-safe record (signal, si_code, addresses, and a frame-pointer backtrace) into the last-session
// status, each address as "<library>+0x<offset>". Privacy: only a library whose name is on the engine/Android
// allow list below may appear by name; any other is written as "app library N", never its name or path.
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace apkrt::crash {

/// Whether a library's soname may appear by name in a crash record. `soname` is the library's name as the
/// loader has it (e.g. "libil2cpp.so", "ld-android.so", "linker64"); a leading "lib" and a trailing ".so"
/// are ignored. A name is kept on an exact match, a match followed by '_' '-' or '.', or a
/// bare prefix for python/icu/v8/fmod/gdx/cocos2d/mono/crashlytics. Any other name is masked ("app library N").
bool kept_soname(std::string_view soname);

/// The allow list itself (sorted, unique, basenames with no "lib" prefix and no ".so"), for its test.
std::span<const std::string_view> keep_libs_list();

/// A loaded module that holds an address, as the loader or the async-signal-safe snapshot resolves it.
struct Module {
    bool found = false;           // false: the address is in no known module
    std::uint64_t base = 0;       // the module's load base, so address - base is the offset
    std::string_view soname;      // e.g. "libil2cpp.so"; shown only when the library is on the allow list
    int app_index = 0;           // a non-allow-listed module's "app library N" number (N >= 1), never its name
};

/// Writes one backtrace frame into out[at..cap), advancing `at`: "<soname>+0x<off>" when the module is on the
/// allow list, "app library <N>+0x<off>" when it is not (privacy: never the soname), "0x<address>" when the
/// address is in no known module. Async-signal-safe: it only writes to the buffer, no allocation.
void format_frame(char *out, std::size_t cap, std::size_t &at, std::uint64_t address, const Module &module);

/// Walks up to `limit` frames of the frame-pointer chain at `fp`, writing each as "#<n> <frame>\n" (see
/// format_frame) into out[at..cap). Fault-safe: each frame record is read with vm_read_overwrite, so an
/// unreadable frame ends the walk instead of faulting; it also ends on a chain that is not strictly upward,
/// is misaligned, or jumps more than 1 MiB (a broken chain), and after `limit` frames. `resolve` maps a
/// return address to its Module and `ctx` is passed through; async-signal-safe when `resolve` is. Returns
/// how many frames it wrote.
int write_backtrace(char *out, std::size_t cap, std::size_t &at, std::uint64_t fp, int limit,
                    Module (*resolve)(std::uint64_t address, void *ctx), void *ctx);

/// A loaded object as the resolver sees it (from dl::mapped_objects at a crash): its host address range, its
/// load bias (a guest vaddr V is at host V + bias, so vaddr = host - bias), and its soname.
struct LoadedObject {
    std::uint64_t image_begin = 0;
    std::uint64_t image_end = 0;
    std::uint64_t bias = 0;
    std::string_view soname;
};

/// Resolves a host address to a Module from the loaded objects in load order: the object that holds it gives
/// the soname and base = its bias (so format_frame's offset is the guest vaddr, matching the ELF, as Android's
/// tombstones print). A non-listed object's app_index counts the non-listed objects in load order (its "app
/// library N"). found = false when no object holds the address. (Original-code addresses; a stub/island
/// address is named by its island's guest site instead -- that path uses the island table.)
Module resolve_in_objects(std::uint64_t address, std::span<const LoadedObject> objects);

/// Registers a loaded module for the async-signal-safe resolver (the signal-frames and fault-diagnostic paths): its
/// host address range, its load bias, and its soname, which it copies (bounded to 63 bytes), so the caller's view need
/// not outlive the call. The loader calls it on each module load; it appends to a fixed, lock-free registry (modules
/// are never unloaded, ADR-0003). It writes the registry, so it is not itself for a signal handler --
/// resolve_registered reads it.
void register_module(std::uint64_t image_begin, std::uint64_t image_end, std::uint64_t bias, std::string_view soname);

/// Resolves a host address over the registered modules (register_module), with the signature write_backtrace's
/// `resolve` takes (`ctx` is unused): the same result as resolve_in_objects over the live objects, but reading the
/// append-only registry with no lock and no allocation, so a signal handler may call it (the signal-frames path).
/// found = false ("0x<address>") when no module holds the address, or when the registry was already full at the load.
Module resolve_registered(std::uint64_t address, void *ctx);

/// Empties the module registry, for tests.
void reset_modules();

/// Writes ONE async-signal-safe diagnostic line for a synchronous fault the runtime is about to deliver to a guest
/// handler -- the app's own crash handler (Unity, Crashlytics, Breakpad), which may end the process itself so the
/// runtime's exit path never writes a status (ADR-0032 decision 8; gated by MINATO_FAULT_LOG in signals.cpp):
///   "E/apkrt: fault <NAME> code <C> at <frame>, addr 0x<addr>, to the app's handler\n"
/// <NAME> is the signal (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP/SIGABRT, else "SIG<n>"), <C> the si_code (signed),
/// <addr> the faulting address (0 for a non-fault terminating signal). <frame> names `pc`'s module as format_frame
/// does, so the privacy mask holds: a non-allow-listed library is "app library N", an address in no module "0x<pc>".
/// Resolves `pc` over the lock-free module registry (register_module) only, so no allocation and no lock the faulting
/// thread may hold; one write() to `fd`. The line is in Minato's liblog form, so the exit path's output capture
/// (process_exit.cpp is_log_line) treats it as the runtime's own words, not the app's.
void write_fault_line(int fd, int signal, int code, std::uint64_t fault_address, std::uint64_t pc);

}  // namespace apkrt::crash
