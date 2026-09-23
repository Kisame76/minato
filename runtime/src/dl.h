// The program's ELF objects and libdl on top of them (ADR-0021, M2 step 5c): what linker64 does in
// a process. It loads the libraries a program needs at start and more at run time (dlopen), finds
// symbols (dlsym), names addresses (dladdr) and lists the objects for dl_iterate_phdr, which
// ld-android.so serves from them. One program per process (ADR-0003): the state is the process's,
// and nothing is ever unloaded.
#pragma once

#include "APKRuntime/guest_interface.h"
#include "linker.h"
#include "loader.h"

#include <filesystem>
#include <string_view>

namespace apkrt::dl {

/// The library containers in `host_directory` (<file name>.dylib, as apk-convert writes them) are the
/// library files the guest finds in `guest_directory`, an absolute guest path, e.g. /system/lib64.
void add_library_directory(std::string_view guest_directory, const std::filesystem::path &host_directory);

/// Loads the libraries the program in `container` needs, breadth first, and links them, as linker64
/// does at start. argv[0] is the program as it was started, e.g. ./name; its guest path is that
/// against the working directory, and the containers next to the program's are the library files in
/// that directory. linker64 looks for a library in LD_LIBRARY_PATH of `envp`, then in /system/lib64
/// and the other default directories of a program in /data/local/tmp. A program that cannot be linked
/// ends the process as with linker64: "CANNOT LINK EXECUTABLE" on stderr, exit status 1.
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

}  // namespace apkrt::dl
