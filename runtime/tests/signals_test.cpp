// Checks guest signal frames against the x18 islands (ADR-0020 X4). XNU clears x18 at every context
// switch, so converted guest code keeps its x18 in Darwin TSD slot 11: a handler must find that value
// in its frame, and rt_sigreturn must give the thread the frame's x18. Likewise PSTATE's DIT, which
// frames keep with SSBS as Linux's do (ADR-0047 b2). The guest threads are clone children that make
// their syscalls through the veneer's exact instructions (threads_probe.S, signals_probe.S).
#include "APKRuntime/apkrt.h"
#include "guest_context.h"
#include "hwcap.h"
#include "threads.h"

#include "check.h"

#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
constexpr std::uint64_t sigusr1 = 10, sigusr2 = 12;
constexpr std::int32_t si_tkill = -6;
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

/// What the handler of the next test found in its siginfo: si_code, si_pid and si_uid.
struct Sender {
    std::atomic<int> handled{0};
    std::atomic<std::int32_t> code{0}, pid{0};
    std::atomic<std::uint32_t> uid{0};
};
Sender sender;

void on_signal_from_sender(int, void *info, void *) {
    std::int32_t code, pid;
    std::uint32_t uid;
    std::memcpy(&code, static_cast<char *>(info) + 8, sizeof code);  // struct siginfo: signo, errno, code
    std::memcpy(&pid, static_cast<char *>(info) + 16, sizeof pid);   // then the union: si_pid, si_uid
    std::memcpy(&uid, static_cast<char *>(info) + 20, sizeof uid);
    sender.code.store(code);
    sender.pid.store(pid);
    sender.uid.store(uid);
    sender.handled.fetch_add(1);
}

void send_to_itself(void *) {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{reinterpret_cast<std::uint64_t>(&on_signal_from_sender), sa_siginfo | sa_restorer,
             reinterpret_cast<std::uint64_t>(&probe_sigreturn), 0};
    probe_syscall(nr_rt_sigaction, sigusr2, reinterpret_cast<std::uint64_t>(&action), 0, 8);
    const long tid = probe_syscall(nr_gettid, 0);
    probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), static_cast<std::uint64_t>(tid), sigusr2);
}

// ADR-0039 decision 3: a signal a thread of the program sends names the process and the app's uid, 10100, as
// its sender, as Linux's do_send_specific fills si_pid and si_uid.
TEST(a_signal_names_the_process_and_the_app_uid_as_its_sender) {
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, send_to_itself, nullptr) > 0);
    join(&tid_word);
    CHECK(sender.handled.load() == 1);
    CHECK(sender.code.load() == si_tkill);
    CHECK(sender.pid.load() == ::getpid());
    CHECK(sender.uid.load() == 10100);
    ::munmap(stack, stack_size);
}

// ADR-0028, changed 2026-10-07: SIGKILL to a thread of the process ends the whole process at once, but only when
// that thread exists. To one that is gone, tgkill and rt_tgsigqueueinfo answer ESRCH, as Linux does, and nothing ends.
constexpr std::uint64_t nr_rt_tgsigqueueinfo = 240, sigkill = 9;
constexpr long esrch = 3;
std::atomic<long> gone_tid{0};
std::atomic<long> gone_tgkill{0};
std::atomic<long> gone_queue{0};

void note_own_tid(void *) {
    gone_tid.store(probe_syscall(nr_gettid, 0));
}

void kill_a_thread_that_is_gone(void *) {
    const auto pid = static_cast<std::uint64_t>(::getpid());
    const auto tid = static_cast<std::uint64_t>(gone_tid.load());
    gone_tgkill.store(probe_syscall(nr_tgkill, pid, tid, sigkill));
    std::uint64_t info[16] = {};  // a struct siginfo of 128 bytes
    gone_queue.store(probe_syscall(nr_rt_tgsigqueueinfo, pid, tid, sigkill, reinterpret_cast<std::uint64_t>(info)));
}

TEST(sigkill_to_a_thread_that_is_gone_answers_esrch) {
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, note_own_tid, nullptr) > 0);
    join(&tid_word);
    CHECK(gone_tid.load() > 0);
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, kill_a_thread_that_is_gone,
                      nullptr) > 0);
    join(&tid_word);
    CHECK(gone_tgkill.load() == -esrch);
    CHECK(gone_queue.load() == -esrch);
    ::munmap(stack, stack_size);
}

// PSTATE.DIT (bit 24) by MRS and MSR, which every Apple Silicon Mac has (FEAT_DIT); SSBS (bit 12) goes the same
// way on a device that has it, which no Mac does (el0-check shows the iPhone's).
constexpr std::uint64_t dit = std::uint64_t{1} << 24;
// The Linux ucontext on arm64: pstate after regs[31], sp and pc.
constexpr std::size_t linux_pstate_offset = linux_regs_offset + 33 * 8;

bool host_has_dit() {
    int value = 0;
    std::size_t size = sizeof value;
    return ::sysctlbyname("hw.optional.arm.FEAT_DIT", &value, &size, nullptr, 0) == 0 && value == 1;
}

std::uint64_t read_dit() {
    std::uint64_t value;
    asm volatile("mrs %0, s3_3_c4_c2_5" : "=r"(value));
    return value & dit;
}

void write_dit(std::uint64_t value) {
    asm volatile("msr s3_3_c4_c2_5, %0" : : "r"(value));
}

/// What the guest thread of the next test saw, per round: the handler clears DIT itself in round 0, and only in its
/// frame in round 1.
struct DitSeen {
    std::atomic<int> handled{0};
    std::atomic<std::uint64_t> at_handler_entry[2]{}, in_frame[2]{}, after_return[2]{};
};
DitSeen dit_seen;

void on_dit_signal(int, void *, void *ucontext) {
    auto *pstate = reinterpret_cast<std::uint64_t *>(static_cast<char *>(ucontext) + linux_pstate_offset);
    const int round = dit_seen.handled.load();
    dit_seen.at_handler_entry[round].store(read_dit());
    dit_seen.in_frame[round].store(*pstate & dit);
    if (round == 0) {
        write_dit(0);
    } else {
        *pstate &= ~dit;
    }
    dit_seen.handled.fetch_add(1);
}

void send_dit_through_a_handler(void *) {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{reinterpret_cast<std::uint64_t>(&on_dit_signal), sa_siginfo | sa_restorer,
             reinterpret_cast<std::uint64_t>(&probe_sigreturn), 0};
    probe_syscall(nr_rt_sigaction, sigusr1, reinterpret_cast<std::uint64_t>(&action), 0, 8);
    const long tid = probe_syscall(nr_gettid, 0);
    for (int round = 0; round < 2; ++round) {
        write_dit(dit);
        // A signal to the thread itself: the runtime delivers it at the end of this syscall.
        probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), static_cast<std::uint64_t>(tid), sigusr1);
        dit_seen.after_return[round].store(read_dit());
    }
    write_dit(0);
}

// ADR-0047 b2: DIT goes through a handler as on Linux, whose frames keep the whole PSTATE (setup_sigframe) and whose
// rt_sigreturn takes it back (restore_sigframe; valid_native_regs leaves bits 12 and 24): the handler starts with the
// interrupted DIT and finds it in its frame; the thread returns with the frame's DIT, not with what the handler left.
TEST(the_signal_frame_carries_dit_and_rt_sigreturn_restores_it) {
    if (!host_has_dit()) return;  // no Apple Silicon Mac lacks it
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, send_dit_through_a_handler,
                      nullptr) > 0);
    join(&tid_word);
    CHECK(dit_seen.handled.load() == 2);
    for (int round = 0; round < 2; ++round) {
        CHECK(dit_seen.at_handler_entry[round].load() == dit);
        CHECK(dit_seen.in_frame[round].load() == dit);
    }
    CHECK(dit_seen.after_return[0].load() == dit);  // the handler's own change ends with it
    CHECK(dit_seen.after_return[1].load() == 0);    // the frame's value is the one that comes back
    ::munmap(stack, stack_size);
}

// The PSTATE bits a guest context keeps besides NZCV are the ones EL0 may write among the features the guest is told:
// SSBS where AT_HWCAP has SSBS (bit 28), DIT where it has DIT (bit 24). The Mac has DIT only, the iPhone both.
TEST(the_kept_pstate_bits_follow_the_reported_features) {
    CHECK(apkrt::kept_pstate_bits(apkrt::hwcap::Bits{0x2fb3f7fb, 0x80386181}) == dit);
    CHECK(apkrt::kept_pstate_bits(apkrt::hwcap::Bits{0x3fb3f7fb, 0x386181}) == (dit | (std::uint64_t{1} << 12)));
    CHECK(apkrt::kept_pstate_bits(apkrt::hwcap::Bits{0x3, 0}) == 0);
    CHECK(apkrt::kept_pstate_bits() == apkrt::kept_pstate_bits(apkrt::hwcap::guest()));
}

}  // namespace

int main() {
    // The test's own thread is the program's first thread: the process lives as long as it does.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    return apk::testing::run_all();
}
