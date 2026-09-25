// Starts a guest program (ADR-0019, ADR-0021): one program per process, on a thread of its own, so
// the main thread stays free for the UI. The thread runs on the program's Linux stack. For a
// dynamic program it does in-process what linker64 does (dl.h): the libraries are loaded and
// linked, libc.so sets up the main thread and its static TLS (__apkrt_start_main_thread), the TLS
// relocations follow, then the constructors of the libraries and the program, then the program
// from its entry point. The program's thread is its first guest thread (threads.h); the process
// ends with exit_group or with its last thread.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"

#include "descriptors.h"
#include "dl.h"
#include "guest_call.h"
#include "loader.h"
#include "process_exit.h"
#include "syscall_frame.h"
#include "threads.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>

namespace {

using apkrt::loader::LoadError;

/// Linux gives the main thread 8 MiB (RLIMIT_STACK) and a guard page below it.
constexpr std::size_t stack_size = 8 << 20;
constexpr std::size_t guard_size = 16 << 10;

/// What the program thread needs. It is never freed.
struct Start {
    bool dynamic = false;
    std::uint64_t entry;
    std::uint64_t stack_pointer;
};

/// Runs on the program thread as its first guest thread, and never returns: the program starts.
void enter_program(void *argument) {
    auto &start = *static_cast<Start *>(argument);
    if (start.dynamic) {
        if (auto *start_main_thread = apkrt::dl::start_main_thread()) {
            auto guest_start = apkrt::dl::guest_start(reinterpret_cast<void *>(start.stack_pointer));
            if (apkrt::call_guest(start_main_thread, &guest_start) != 0) {
                static const char message[] = "apkrt: too many objects with TLS for the main thread\n";
                static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
                apkrt::process::exit(127);
            }
        }
        // Constructors get argc, argv and envp from the start stack, as linker64 passes them.
        auto *const words = reinterpret_cast<std::uint64_t *>(start.stack_pointer);
        const auto argc = static_cast<int>(words[0]);
        auto **const argv = reinterpret_cast<char **>(words + 1);
        apkrt::dl::finish_start(argc, argv, argv + argc + 1);
    }
    apkrt::signals::enter_guest(start.entry, start.stack_pointer);
}

void *program_thread(void *argument) {
    // The islands of converted code keep the guest thread pointer in TSD slot 6 and x18 in slot 11
    // (ADR-0019, ADR-0020). A new thread must start with both empty: the program's libc sets the
    // thread pointer (msr TPIDR_EL0 becomes an island that writes it), and x18 starts as 0, as
    // Linux's start_thread clears every register.
    if (const char *slot = apkrt::threads::used_guest_slot()) {
        const auto message = std::format("apkrt: {} is already in use on the program thread\n", slot);
        static_cast<void>(::write(STDERR_FILENO, message.data(), message.size()));
        apkrt::process::exit(127);
    }
    static apkrt::threads::GuestThread first;
    first.tid = ::getpid();  // on Linux the first thread's ID is the process ID
    apkrt::threads::add(first);
    apkrt::threads::run(first, enter_program, argument);
    return nullptr;  // the first thread ended with SYS_exit; the process lives on with the others
}

std::vector<std::string> strings(const char *const list[]) {
    std::vector<std::string> result;
    for (; list != nullptr && *list != nullptr; ++list) result.emplace_back(*list);
    return result;
}

[[noreturn]] void run(const char *container_path, const char *const argv[], const char *const envp[]) {
    // The program's 0, 1 and 2 are duplicates of the starter's (ADR-0027).
    apkrt::descriptors::adopt_standard_descriptors();
    const auto image = apkrt::loader::load(container_path);
    const auto &info = *image.info;
    const bool is_static = (info.flags & APK_CONTAINER_STATIC) != 0;
    auto *start = new Start{};
    start->dynamic = !is_static;
    if (is_static) {
        apkrt::loader::relocate(image);
    } else {
        apkrt::dl::load_program(container_path, image, argv, envp);
    }

    void *const mapping = ::mmap(nullptr, stack_size + guard_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) throw LoadError(std::format("no memory for the stack: {}", std::strerror(errno)));
    if (::mprotect(mapping, guard_size, PROT_NONE) != 0) {
        throw LoadError(std::format("cannot protect the stack guard: {}", std::strerror(errno)));
    }
    const std::span stack(static_cast<std::byte *>(mapping) + guard_size, stack_size);

    const apkrt::loader::StartInfo start_info{
        .arguments = strings(argv),
        .environment = strings(envp),
        .program_headers = std::span(reinterpret_cast<const std::byte *>(image.bias + info.phdr),
                                     std::size_t{info.phnum} * info.phentsize),
        .program_header_count = info.phnum,
        .bias = image.bias,
        .entry = image.bias + info.entry,
        .static_program = is_static,
    };
    start->entry = start_info.entry;
    start->stack_pointer = apkrt::loader::build_start_stack(start_info, stack);

    // The thread's own frames go below the start stack, on whole pages of the same mapping.
    const auto page = static_cast<std::uint64_t>(::getpagesize());
    const auto base = reinterpret_cast<std::uint64_t>(stack.data());
    const auto top = start->stack_pointer & ~(page - 1);
    pthread_attr_t attributes;
    ::pthread_attr_init(&attributes);
    if (const int error = ::pthread_attr_setstack(&attributes, stack.data(), top - base); error != 0) {
        throw LoadError(std::format("cannot give the program thread its stack: {}", std::strerror(error)));
    }
    pthread_t thread;
    if (const int error = ::pthread_create(&thread, &attributes, program_thread, start); error != 0) {
        throw LoadError(std::format("cannot create the program thread: {}", std::strerror(error)));
    }
    ::pthread_attr_destroy(&attributes);
    ::pthread_join(thread, nullptr);
    // The program's first thread has ended, the others go on; the last one ends the process.
    for (;;) ::pause();
}

}  // namespace

extern "C" int apkrt_run_program(const char *container_path, const char *const argv[], const char *const envp[],
                                 char *error, size_t error_size) {
    try {
        run(container_path, argv, envp);
    } catch (const std::exception &failure) {
        if (error != nullptr && error_size > 0) std::snprintf(error, error_size, "%s", failure.what());
    }
    return -1;
}
