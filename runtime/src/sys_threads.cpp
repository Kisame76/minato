// Thread syscalls of the Linux syscall layer (M2 step 5c, ARCHITECTURE.md 4.5): clone for new
// threads, as bionic's pthread_create uses it, and the thread IDs. Processes cannot be cloned: iOS
// has no fork (F11).
#include "guest_context.h"
#include "signals.h"
#include "syscalls.h"
#include "threads.h"

#include "APKRuntime/apkrt.h"

#include <pthread.h>
#include <unistd.h>

namespace apkrt::sys {
namespace {

// include/uapi/linux/sched.h
constexpr std::uint64_t clone_vm = 0x100, clone_fs = 0x200, clone_files = 0x400, clone_sighand = 0x800,
                        clone_vfork = 0x4000, clone_thread = 0x10000, clone_settls = 0x80000,
                        clone_parent_settid = 0x100000, clone_child_cleartid = 0x200000,
                        clone_child_settid = 0x1000000;
constexpr std::uint64_t thread_flags = clone_vm | clone_fs | clone_files | clone_sighand | clone_thread;

void *start_child(void *argument) {
    auto *thread = static_cast<threads::GuestThread *>(argument);
    threads::run(
        *thread, [](void *child) { signals::resume(static_cast<threads::GuestThread *>(child)->start); },
        thread);
    delete thread;
    return nullptr;
}

}  // namespace

long sys_clone(const apkrt_syscall_frame &frame, std::uint64_t flags, std::uint64_t stack, std::uint64_t parent_tid,
               std::uint64_t tls, std::uint64_t child_tid) {
    if ((flags & thread_flags) != thread_flags || (flags & clone_vfork) != 0) {
        report("apkrt: clone for a new process (fork, vfork) is not possible on iOS\n");
        return -linux_errno::enosys;
    }
    auto start = context_at_syscall(frame);
    if (!start) {
        report("apkrt: clone from outside a syscall veneer\n");
        return -linux_errno::enosys;
    }
    start->x[0] = 0;  // the child's result
    if (stack != 0) start->sp = stack;

    auto *thread = new threads::GuestThread{};
    thread->tid = threads::new_tid();
    thread->start = *start;
    thread->thread_pointer = (flags & clone_settls) ? tls : reinterpret_cast<std::uint64_t>(apkrt_get_guest_tls());
    thread->x18 = apkrt_get_guest_x18();  // the child starts with the parent's registers, x18 included
    if (flags & clone_child_cleartid) thread->clear_child_tid = child_tid;
    // The child's signal mask is the parent's; its alternate signal stack is none (CLONE_VM). Its nice value is the
    // parent's (copy_process).
    thread->signals.mask.store(signals::current().mask.load());
    if (const threads::GuestThread *parent = threads::current()) thread->nice.store(parent->nice.load());
    // Both IDs are written before the child runs; it shares the parent's memory.
    if (flags & clone_parent_settid) *as_pointer<std::int32_t>(parent_tid) = thread->tid;
    if (flags & clone_child_settid) *as_pointer<std::int32_t>(child_tid) = thread->tid;
    threads::add(*thread);
    const std::int32_t tid = thread->tid;  // the child may be gone before pthread_create returns

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t host;
    const int error = pthread_create(&host, &attributes, start_child, thread);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        threads::forget(*thread);
        delete thread;
        return -linux_errno::eagain;
    }
    return tid;
}

long sys_set_tid_address(std::uint64_t address) {
    threads::GuestThread *thread = threads::current();
    if (thread == nullptr) return ::getpid();
    thread->clear_child_tid = address;
    return thread->tid;
}

long sys_gettid() {
    const threads::GuestThread *thread = threads::current();
    return thread != nullptr ? thread->tid : ::getpid();
}

}  // namespace apkrt::sys
