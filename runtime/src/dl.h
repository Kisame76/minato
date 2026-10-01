// The program's ELF objects and libdl on top of them (ADR-0021, M2 step 5c): what linker64 does in
// a process. It loads the libraries a program needs at start and more at run time (dlopen), finds
// symbols (dlsym), names addresses (dladdr) and lists the objects for dl_iterate_phdr, which
// ld-android.so serves from them. One program per process (ADR-0003): the state is the process's,
// and nothing is ever unloaded.
#pragma once

#include "APKRuntime/guest_interface.h"
#include "linker.h"
#include "loader.h"
#include "syscall_frame.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace apkrt::dl {

/// Points libdl's functions in ld-android.so's table at the loader (load_program does it): at the
/// stubs in syscall_entry.S, which make them syscalls of the runtime's own (ADR-0025).
void set_loader_functions(apkrt_loader &loader);

/// The syscalls of those stubs (loader_syscalls.h), with their arguments in x0-x3 as the C
/// functions of apkrt_loader take them; the result goes back in x0.
long loader_syscall(const apkrt_syscall_frame &frame);

/// The lock of the loader's state, which its functions and the program's start take, recursively as
/// linker64's g_dl_mutex. A test holds it to keep a guest thread inside the loader.
std::recursive_mutex &lock();

/// The library containers in `host_directory` (<file name>.dylib, as apk-convert writes them) are the
/// library files the guest finds in `guest_directory`, an absolute guest path, e.g. /system/lib64.
void add_library_directory(std::string_view guest_directory, const std::filesystem::path &host_directory);

/// Loads the libraries the program in `container` needs, breadth first, and links them, as linker64
/// does at start. argv[0] is the program as it was started, e.g. ./name; its guest path is that
/// against the working directory, and the containers next to the program's are the library files in
/// that directory. linker64 looks for a library in LD_LIBRARY_PATH of `envp`, then in /system/lib64
/// and the other default directories of a program in /data/local/tmp. A program that cannot be linked
/// ends as with linker64: "CANNOT LINK EXECUTABLE" on stderr, exit status 1 (process::end). On the
/// program's thread: the ifunc resolvers are guest code.
void load_program(const std::filesystem::path &container, const loader::Image &image, const char *const argv[],
                  const char *const envp[]);

/// libc.so's __apkrt_start_main_thread, or null when the program has no bionic libc.
apkrt_start_main_thread_function start_main_thread();

/// The objects as the guest sees them, for __apkrt_start_main_thread, which writes their TLS
/// modules into them.
apkrt_start guest_start(void *start_stack);

/// On the program thread, after __apkrt_start_main_thread (if any): the TLS relocations, then the
/// constructors, with the program's argc, argv and envp, which dlopen'ed libraries get as well.
/// Guest code runs; on failure the process ends with a message, as linker64 ends it.
void finish_start(int argc, char **argv, char **envp);

/// A loaded ELF object as /proc/self/maps shows it (proc.h).
struct MappedObject {
    std::string path;                       // the guest path linker64 reports
    std::filesystem::path container;
    std::uint64_t bias = 0;
    std::uint64_t image_begin = 0;          // host addresses of the image, page aligned
    std::uint64_t image_end = 0;
    const void *program_headers = nullptr;  // Elf64_Phdr[program_header_count], or null when no
    std::size_t program_header_count = 0;   // segment holds them
};

/// The program and its libraries, in load order.
std::vector<MappedObject> mapped_objects();

}  // namespace apkrt::dl
