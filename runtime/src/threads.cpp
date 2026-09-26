#include "threads.h"

#include "APKRuntime/apkrt.h"
#include "process_exit.h"
#include "syscalls.h"

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>

/// current_key for syscall_entry.S, which reads the GuestThread from the TSD itself (ADR-0025); ~0
/// until the key exists.
extern "C" std::atomic<std::uint64_t> apkrt_guest_thread_key{~std::uint64_t{0}};
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && std::atomic<std::uint64_t>::is_always_lock_free);

namespace apkrt::threads {
namespace {

std::once_flag key_created;
pthread_key_t current_key;

constexpr std::size_t host_signal_stack_size = 64 * 1024;

std::mutex registry_mutex;
std::map<std::int32_t, GuestThread *> registry;  // guarded by registry_mutex
int living = 0;                                  // guarded by registry_mutex
int first_thread_exit_code = 0;                  // guarded by registry_mutex

void create_key() {
    pthread_key_create(&current_key, nullptr);
    apkrt_guest_thread_key.store(current_key);
}

/// What the kernel does when a thread has ended, once the thread is off the guest's stack.
void finish(GuestThread &thread) {
    pthread_setspecific(current_key, nullptr);
    apkrt_set_guest_tls(nullptr);
    apkrt_set_guest_x18(0);
    if (thread.host_signal_stack != nullptr) {
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        off.ss_size = host_signal_stack_size;  // Darwin wants a size even here
        ::sigaltstack(&off, nullptr);
        ::munmap(thread.host_signal_stack, host_signal_stack_size);
        thread.host_signal_stack = nullptr;
    }
    if (thread.deferred_unmap_length != 0) {
        ::munmap(reinterpret_cast<void *>(thread.deferred_unmap_start), thread.deferred_unmap_length);
    }
    if (thread.clear_child_tid != 0) {
        // CLONE_CHILD_CLEARTID: pthread_join waits for this word to become 0, as a shared futex.
        auto *word = reinterpret_cast<std::uint32_t *>(thread.clear_child_tid);
        __atomic_store_n(word, 0, __ATOMIC_RELEASE);
        sys::futex_wake(word, 1, true);
    }
    bool last;
    int code;
    {
        const std::lock_guard lock(registry_mutex);
        registry.erase(thread.tid);
        last = --living == 0;
        if (thread.tid == ::getpid()) first_thread_exit_code = thread.exit_code;
        code = first_thread_exit_code;
    }
    // On Linux the process ends with its last thread and reports the first thread's exit code.
    if (last) process::end(code);
}

}  // namespace

GuestThread *current() {
    std::call_once(key_created, create_key);
    return static_cast<GuestThread *>(pthread_getspecific(current_key));
}

const char *used_guest_slot() {
    if (apkrt_get_guest_tls() != nullptr) return "Darwin TSD slot 6";
    if (apkrt_get_guest_x18() != 0) return "Darwin TSD slot 11";
    return nullptr;
}

std::int32_t new_tid() {
    static std::atomic<std::int32_t> next{1};
    return ::getpid() + next.fetch_add(1);
}

void add(GuestThread &thread) {
    const std::lock_guard lock(registry_mutex);
    registry[thread.tid] = &thread;
    ++living;
}

void find(std::int32_t tid, const std::function<void(GuestThread &)> &visit) {
    const std::lock_guard lock(registry_mutex);
    if (const auto found = registry.find(tid); found != registry.end()) visit(*found->second);
}

void for_each(const std::function<void(GuestThread &)> &visit) {
    const std::lock_guard lock(registry_mutex);
    for (auto &[tid, thread] : registry) visit(*thread);
}

void forget(GuestThread &thread) {
    const std::lock_guard lock(registry_mutex);
    registry.erase(thread.tid);
    --living;
}

void run(GuestThread &thread, void (*enter)(void *), void *argument) {
    std::call_once(key_created, create_key);
    signals::install();
    pthread_setspecific(current_key, &thread);
    thread.signals.host = pthread_self();
    // The runtime's signal handlers run on a stack of their own: a guest thread may have used up
    // its stack when a fault arrives. A stack the thread has already (AddressSanitizer gives every
    // thread one, and frees it at the thread's end) serves as well.
    stack_t present{};
    ::sigaltstack(nullptr, &present);
    if ((present.ss_flags & SS_DISABLE) != 0) {
        void *memory = ::mmap(nullptr, host_signal_stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (memory != MAP_FAILED) {
            stack_t stack{};
            stack.ss_sp = memory;
            stack.ss_size = host_signal_stack_size;
            if (::sigaltstack(&stack, nullptr) == 0) {
                thread.host_signal_stack = memory;
            } else {
                ::munmap(memory, host_signal_stack_size);
            }
        }
    }
    apkrt_set_guest_tls(reinterpret_cast<void *>(thread.thread_pointer));
    apkrt_set_guest_x18(thread.x18);
    if (apkrt_save_host(&thread.exit_point) == 0) {
        enter(argument);
        static const char message[] = "apkrt: guest code returned to the host\n";
        static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
        process::exit(127);
    }
    finish(thread);
}

void exit_current(int code) {
    GuestThread *thread = current();
    if (thread == nullptr) process::exit(code);  // a host thread that made a guest syscall (tests)
    thread->exit_code = code;
    apkrt_return_to_host(&thread->exit_point);
}

}  // namespace apkrt::threads
