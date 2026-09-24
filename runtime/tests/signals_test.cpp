// Checks guest signal frames against the x18 islands (ADR-0020 X4). XNU clears x18 at every context
// switch, so converted guest code keeps its x18 in Darwin TSD slot 11: a handler must find that value
// in its frame, and rt_sigreturn must give the thread the frame's x18. The guest threads are clone
// children that make their syscalls through the veneer's exact instructions (threads_probe.S,
// signals_probe.S).
#include "APKRuntime/apkrt.h"
#include "threads.h"

#include "check.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
// probe_clone loads x19-x28 and v8-v15 from probe_marks and stores the child's into
// probe_child_registers (threads_test.cpp checks them; here they only have to exist).
struct ProbeRegisters {
    std::uint64_t x[10];
    __uint128_t q[8];
    std::uint64_t sp;
};
ProbeRegisters probe_marks;
ProbeRegisters probe_child_registers;

long probe_clone(std::uint64_t flags, void *child_stack, int *parent_tid, void *tls, int *child_tid,
                 void (*function)(void *), void *argument);
long probe_syscall(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                   std::uint64_t a3 = 0);
void probe_sigreturn();
}

namespace {

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, asm-generic/signal.h, errno.h.
constexpr std::uint64_t pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr std::uint64_t nr_futex = 98, nr_tgkill = 131, nr_rt_sigaction = 134, nr_gettid = 178;
constexpr std::uint64_t sigusr1 = 10;
constexpr std::uint64_t sa_siginfo = 0x4, sa_restorer = 0x0400'0000;
constexpr long eagain = 11;
// The Linux ucontext on arm64: struct sigcontext's regs[31] after fault_address (signals.cpp).
constexpr std::size_t linux_regs_offset = 184;

constexpr std::size_t stack_size = 256 * 1024;

void *map_stack() {
    void *stack = ::mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return stack == MAP_FAILED ? nullptr : stack;
}

void *top_of(void *stack) {
    return static_cast<char *>(stack) + stack_size;
}

/// pthread_join's wait: until the kernel clears the thread ID word at the thread's exit.
void join(int *tid_word) {
    for (int value; (value = __atomic_load_n(tid_word, __ATOMIC_ACQUIRE)) != 0;) {
        const long result = probe_syscall(nr_futex, reinterpret_cast<std::uint64_t>(tid_word), 0 /* FUTEX_WAIT */,
                                          static_cast<std::uint64_t>(value), 0);
        CHECK(result == 0 || result == -eagain);
    }
}

constexpr std::uint64_t x18_before = 0xa18a'18a1'8a18'a18a;    // the guest's x18 when the signal comes
constexpr std::uint64_t x18_returned = 0xb18b'18b1'8b18'b18b;  // what the handler writes into its frame
constexpr std::uint64_t x18_in_handler = 0xdead;               // what the handler leaves in x18 itself

/// What the guest thread saw.
struct Seen {
    std::atomic<int> handled{0};
    std::atomic<std::uint64_t> at_handler_entry{0};
    std::atomic<std::uint64_t> in_frame{0};
    std::atomic<std::uint64_t> after_return{0};
};
Seen seen;

void on_signal(int, void *, void *ucontext) {
    auto *regs = reinterpret_cast<std::uint64_t *>(static_cast<char *>(ucontext) + linux_regs_offset);
    seen.at_handler_entry.store(apkrt_get_guest_x18());
    seen.in_frame.store(regs[18]);
    regs[18] = x18_returned;
    apkrt_set_guest_x18(x18_in_handler);  // a handler may use x18 freely, as on Linux
    seen.handled.fetch_add(1);
}

void send_x18_through_a_handler(void *) {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{reinterpret_cast<std::uint64_t>(&on_signal), sa_siginfo | sa_restorer,
             reinterpret_cast<std::uint64_t>(&probe_sigreturn), 0};
    probe_syscall(nr_rt_sigaction, sigusr1, reinterpret_cast<std::uint64_t>(&action), 0, 8);
    apkrt_set_guest_x18(x18_before);
    const long tid = probe_syscall(nr_gettid, 0);
    // A signal to the thread itself: the runtime delivers it at the end of this syscall.
    probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), static_cast<std::uint64_t>(tid), sigusr1);
    seen.after_return.store(apkrt_get_guest_x18());
}

// ADR-0020 X4: a handler starts with the interrupted x18, as on Linux, and finds it in its frame;
// rt_sigreturn gives the thread the frame's x18, not whatever the handler left in x18.
TEST(the_signal_frame_carries_x18) {
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, send_x18_through_a_handler,
                      nullptr) > 0);
    join(&tid_word);
    CHECK(seen.handled.load() == 1);
    CHECK(seen.at_handler_entry.load() == x18_before);
    CHECK(seen.in_frame.load() == x18_before);
    CHECK(seen.after_return.load() == x18_returned);
    ::munmap(stack, stack_size);
}

}  // namespace

int main() {
    // The test's own thread is the program's first thread: the process lives as long as it does.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    return apk::testing::run_all();
}
