// Checks the switches between guest code and runtime code (ADR-0025): a guest handler only ever runs
// on the state of guest code, and a signal that arrives while the runtime works waits until the thread
// goes back to guest code, without getting lost on the way. The guest threads are clone children that
// make their syscalls through the veneer's exact instructions (threads_probe.S, transitions_probe.S);
// the signals come from the test's own thread, like from another thread of the program.
#include "dl.h"
#include "linker.h"
#include "signals.h"
#include "syscall_frame.h"
#include "threads.h"

#include "check.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

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
void probe_syscall_loop(std::uint64_t rounds, std::uint64_t pid, std::uint64_t tid, const std::uint64_t *set);
// syscall_entry_probe.S: its state has the layout of the frame.
void probe_syscall_entry(std::uint64_t number, const apkrt_syscall_frame *before, apkrt_syscall_frame *after);
extern const std::uint32_t probe_loop_begin[];
extern const std::uint32_t probe_loop_end[];
long probe_syscall_without_veneer(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                                  std::uint64_t a3 = 0);
std::uint64_t probe_call(const void *function, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                         std::uint64_t a3 = 0);
extern const std::uint32_t probe_call_return[];
extern const std::uint32_t probe_site[];
extern const std::uint32_t probe_after_site[];
extern const std::uint32_t probe_stub_return[];
}

namespace {

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, asm-generic/signal.h, errno.h.
constexpr std::uint64_t pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr std::uint64_t nr_futex = 98, nr_tgkill = 131, nr_rt_sigaction = 134,
                        nr_rt_sigprocmask = 135, nr_gettid = 178;
constexpr std::uint64_t sigusr1 = 10, sigusr2 = 12;
constexpr std::uint64_t nr_read = 63;
constexpr std::uint64_t sa_siginfo = 0x4, sa_restorer = 0x0400'0000, sa_restart = 0x1000'0000;
constexpr std::uint64_t sig_block = 0, sig_unblock = 1;
constexpr long esrch = 3, eintr = 4, eagain = 11;
// The Linux ucontext on arm64: struct sigcontext's regs[31] after fault_address, then sp and pc.
constexpr std::size_t linux_regs_offset = 184, linux_pc_offset = 440;

constexpr std::uint64_t bit(std::uint64_t signal) {
    return std::uint64_t{1} << (signal - 1);
}

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

/// What the handler saw: how often each signal came, and with `recording` the pc of every frame.
struct Seen {
    std::atomic<bool> recording{false};
    std::atomic<int> count[64] = {};
    static constexpr int capacity = 1 << 16;
    std::atomic<int> pcs_recorded{0};
    std::uint64_t pcs[capacity] = {};
};
Seen seen;

void on_signal(int signal, void *, void *ucontext) {
    if (signal > 0 && signal < 64) seen.count[signal].fetch_add(1);
    if (!seen.recording.load()) return;
    const int index = seen.pcs_recorded.fetch_add(1);
    if (index < Seen::capacity) {
        seen.pcs[index] = *reinterpret_cast<const std::uint64_t *>(static_cast<const char *>(ucontext) + linux_pc_offset);
    }
}

/// SIGUSR1 and SIGUSR2 go to on_signal with SA_RESTART, each blocking both while it runs.
void install_handlers() {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{reinterpret_cast<std::uint64_t>(&on_signal), sa_siginfo | sa_restorer | sa_restart,
             reinterpret_cast<std::uint64_t>(&probe_sigreturn), bit(sigusr1) | bit(sigusr2)};
    for (const auto signal : {sigusr1, sigusr2}) {
        CHECK(probe_syscall(nr_rt_sigaction, signal, reinterpret_cast<std::uint64_t>(&action), 0, 8) == 0);
    }
}

/// The test thread's own signal mask, which a clone child starts with.
void set_own_mask(std::uint64_t how, std::uint64_t set) {
    CHECK(probe_syscall(nr_rt_sigprocmask, how, reinterpret_cast<std::uint64_t>(&set), 0, 8) == 0);
}

long send(long tid, std::uint64_t signal) {
    return probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), static_cast<std::uint64_t>(tid), signal);
}

/// Busy-waits for about `nanoseconds`, so that signals land at every point of a syscall.
void spin(std::int64_t nanoseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(nanoseconds);
    while (std::chrono::steady_clock::now() < until) {
    }
}

std::uint64_t next_random(std::uint64_t &state) {  // xorshift64
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

struct LoopView {
    std::atomic<long> tid{0};
    std::atomic<int> done{0};
};

void loop_child(void *argument) {
    auto &view = *static_cast<LoopView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    const std::uint64_t set = bit(sigusr1);
    probe_syscall_loop(20'000, static_cast<std::uint64_t>(::getpid()), static_cast<std::uint64_t>(view.tid.load()), &set);
    view.done = 1;
}

// ADR-0025, cases 1 and 4: wherever a signal from another thread finds the thread (in guest code, in
// the entry or exit of a syscall, while the runtime resumes it for a handler or after rt_sigreturn),
// the handler's frame holds a pc of the guest's own code.
TEST(a_handler_only_ever_sees_guest_code) {
    seen.pcs_recorded = 0;
    const int remote_before = seen.count[sigusr1].load();
    set_own_mask(sig_block, bit(sigusr1));  // the child starts with SIGUSR1 blocked, the loop unblocks it
    void *stack = map_stack();
    CHECK(stack != nullptr);
    static LoopView view;
    view.tid = 0;
    view.done = 0;
    int tid_word = 0;
    seen.recording = true;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, loop_child, &view) > 0);
    while (view.tid.load() == 0) std::this_thread::yield();
    std::uint64_t random = 0x2545'f491'4f6c'dd1d;
    while (view.done.load() == 0) {
        const long sent = send(view.tid.load(), sigusr1);
        CHECK(sent == 0 || (sent == -esrch && view.done.load() != 0));  // the child may just have ended
        spin(static_cast<std::int64_t>(next_random(random) % 20'000));
    }
    join(&tid_word);
    seen.recording = false;
    set_own_mask(sig_unblock, bit(sigusr1));
    ::munmap(stack, stack_size);

    const int remote = seen.count[sigusr1].load() - remote_before;
    CHECK(remote >= 100);  // enough signals from outside came while the loop ran
    const int recorded = std::min(seen.pcs_recorded.load(), Seen::capacity);
    const auto begin = reinterpret_cast<std::uint64_t>(probe_loop_begin);
    const auto end = reinterpret_cast<std::uint64_t>(probe_loop_end);  // a delivery after the last syscall
    int outside = 0;
    for (int index = 0; index < recorded; ++index) {
        const auto pc = seen.pcs[index];
        if (pc >= begin && pc <= end) continue;
        if (++outside <= 5) {
            Dl_info info{};
            const bool named = ::dladdr(reinterpret_cast<const void *>(pc), &info) != 0 && info.dli_sname != nullptr;
            std::fprintf(stderr, "a handler ran at pc %#llx (%s+%#llx), outside the guest's loop\n",
                         static_cast<unsigned long long>(pc), named ? info.dli_sname : "?",
                         named ? static_cast<unsigned long long>(pc - reinterpret_cast<std::uint64_t>(info.dli_saddr)) : 0ull);
        }
    }
    if (outside > 0) std::fprintf(stderr, "%d of %d frames outside the guest's loop\n", outside, recorded);
    CHECK(outside == 0);
}

/// Every register different, as syscall_entry_test.c chooses them, with the flags given.
apkrt_syscall_frame distinct_state(std::uint64_t nzcv, std::uint64_t fpsr) {
    apkrt_syscall_frame state{};
    for (int index = 0; index < 32; ++index) {
        state.x[index] = 0x0101'0101'0101'0101ull * static_cast<std::uint64_t>(index + 1) ^ 0x8000'0000'0000'0000ull;
        state.q[index] = (static_cast<__uint128_t>(0xa5a5'a5a5'a5a5'a5a5ull + static_cast<std::uint64_t>(index)) << 64) |
                         static_cast<std::uint64_t>(index * 977 + 1);
    }
    state.nzcv = nzcv;
    state.fpsr = fpsr;
    return state;
}

struct KeptView {
    std::atomic<long> tid{0};
    apkrt_syscall_frame before[2];
    apkrt_syscall_frame after[2];
};

void kept_child(void *argument) {
    auto &view = *static_cast<KeptView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    for (int index = 0; index < 2; ++index) probe_syscall_entry(nr_gettid, &view.before[index], &view.after[index]);
}

// ADR-0025: on a guest thread, the exit clears in_runtime and looks at the pending signals once more
// before it returns. Like the Linux kernel it keeps every register but x0, the flags included; x16
// and x30 belong to the veneer, x18 to XNU (F4).
TEST(a_syscall_on_a_guest_thread_keeps_every_register_but_x0) {
    static KeptView view;
    view.tid = 0;
    view.before[0] = distinct_state(0x6000'0000, 0x0800'009f);  // Z and C, QC and every exception flag
    view.before[1] = distinct_state(0x9000'0000, 0);            // N and V
    for (auto &state : view.before) state.x[8] = nr_gettid;     // the probe calls with it
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, kept_child, &view) > 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    for (int state = 0; state < 2; ++state) {
        const auto &before = view.before[state];
        const auto &after = view.after[state];
        CHECK(static_cast<long>(after.x[0]) == view.tid.load());
        for (int index = 1; index < 30; ++index) {
            if (index == 16 || index == 18) continue;
            if (before.x[index] != after.x[index]) std::fprintf(stderr, "state %d: x%d changed\n", state, index);
            CHECK(before.x[index] == after.x[index]);
        }
        for (int index = 0; index < 32; ++index) CHECK(before.q[index] == after.q[index]);
        if (before.nzcv != after.nzcv) {
            std::fprintf(stderr, "state %d: NZCV changed from %#llx to %#llx\n", state,
                         static_cast<unsigned long long>(before.nzcv), static_cast<unsigned long long>(after.nzcv));
        }
        CHECK(before.nzcv == after.nzcv);
        CHECK(before.fpsr == after.fpsr);
    }
}

struct SpinView {
    std::atomic<long> tid{0};
    std::atomic<int> spinning{0};
    std::atomic<int> arrived{0};
};

/// Waits without a syscall until a SIGUSR1 came, for 5 s at most.
void spin_for_signal(SpinView &view) {
    const int before = seen.count[sigusr1].load();
    view.spinning = 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (seen.count[sigusr1].load() == before && std::chrono::steady_clock::now() < deadline) {
    }
    view.arrived = static_cast<int>(seen.count[sigusr1].load() != before);
}

void spin_child(void *argument) {
    auto &view = *static_cast<SpinView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    // A handler first, so that the thread's last switch into guest code was the resume after
    // rt_sigreturn; from then on it makes no syscall.
    const int own = seen.count[sigusr2].load();
    send(view.tid.load(), sigusr2);
    CHECK(seen.count[sigusr2].load() == own + 1);
    spin_for_signal(view);
}

void spin_after_bare_syscall_child(void *argument) {
    auto &view = *static_cast<SpinView *>(argument);
    view.tid = probe_syscall_without_veneer(nr_gettid, 0);
    spin_for_signal(view);
}

/// Runs `child` on a clone child and sends it SIGUSR1 once it spins; true when the signal arrived.
bool signal_reaches_spinning_child(void (*child)(void *)) {
    static SpinView view;
    view.tid = 0;
    view.spinning = 0;
    view.arrived = 0;
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, child, &view) > 0);
    while (view.spinning.load() == 0) std::this_thread::yield();
    CHECK(send(view.tid.load(), sigusr1) == 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    return view.arrived.load() == 1;
}

// ADR-0025, decision 1: a resume clears in_runtime when the thread gets its context, so a signal from
// another thread reaches guest code that makes no syscall.
TEST(a_signal_reaches_guest_code_that_makes_no_syscall) {
    CHECK(signal_reaches_spinning_child(spin_child));
}

// A syscall that did not come through a veneer returns to guest code as well; only a signal at its
// end has to wait for the next syscall.
TEST(a_syscall_without_a_veneer_still_returns_to_guest_code) {
    CHECK(signal_reaches_spinning_child(spin_after_bare_syscall_child));
}

std::uint64_t address(const void *label) {
    return reinterpret_cast<std::uint64_t>(label);
}

// ADR-0025, decision 3: by its pc and in_runtime, a runtime signal tells the syscall entry before it
// set in_runtime (the signal waits) and the exit after it cleared it (counts as returned) from
// guest code. The words are the ones syscall_entry.S has between its labels.
TEST(a_runtime_signal_knows_the_edges_of_a_syscall) {
    using apkrt::signals::Stopped;
    using apkrt::signals::where_stopped;
    const auto entry = reinterpret_cast<std::uint64_t>(&apkrt_syscall_entry);
    const auto entered = address(apkrt_syscall_entered);
    const auto returning = address(apkrt_syscall_returning);
    const auto returned = address(apkrt_syscall_returned);
    CHECK(where_stopped(entry, false) == Stopped::in_runtime);        // not even the frame yet
    CHECK(where_stopped(entered - 4, false) == Stopped::in_runtime);  // the store of in_runtime
    CHECK(where_stopped(entered, true) == Stopped::in_runtime);
    CHECK(where_stopped(returning - 4, true) == Stopped::in_runtime);  // the clear, not yet done
    CHECK(where_stopped(returning, false) == Stopped::returning);
    CHECK(where_stopped(address(apkrt_syscall_return), false) == Stopped::returning);
    CHECK(where_stopped(returned - 4, false) == Stopped::returning);  // the slow path, before its set
    CHECK(where_stopped(returned, true) == Stopped::in_runtime);
    CHECK(where_stopped(address(probe_loop_begin), false) == Stopped::in_guest_code);
    CHECK(where_stopped(address(probe_loop_begin), true) == Stopped::in_runtime);
    // apkrt_call_guest's own words are runtime code: after the guest code returned, in_runtime is
    // still clear until the helper sets it.
    CHECK(where_stopped(reinterpret_cast<std::uint64_t>(&apkrt_call_guest), false) == Stopped::in_runtime);
    CHECK(where_stopped(address(apkrt_call_guest_end) - 4, false) == Stopped::in_runtime);
    // The loader's stubs: on the way in the thread counts as in the runtime already, on the way out
    // as back at its caller.
    CHECK(where_stopped(reinterpret_cast<std::uint64_t>(&apkrt_loader_dladdr), false) == Stopped::in_runtime);
    CHECK(where_stopped(address(apkrt_loader_back) - 4, false) == Stopped::in_runtime);  // the blr
    CHECK(where_stopped(address(apkrt_loader_back), false) == Stopped::leaving_loader);
    CHECK(where_stopped(address(apkrt_loader_return), false) == Stopped::leaving_loader);
}

// ADR-0025: a thread stopped in a loader stub after its call returned gets its caller's state, as if
// the stub had run to its end: x16 and x30 from the stub's words, sp above them, pc = x30.
TEST(a_thread_stopped_on_its_way_out_of_the_loader_is_back_at_the_caller) {
    alignas(16) static std::uint64_t saved[2];
    saved[0] = 0x1616;
    saved[1] = address(probe_call_return);  // the caller's return address, which the stub saved
    _STRUCT_MCONTEXT64 machine{};
    ucontext_t uc{};
    uc.uc_mcontext = &machine;
    machine.__ss.__x[0] = 7;  // the result
    machine.__ss.__x[19] = 0x1919;
    machine.__ss.__pc = address(apkrt_loader_back);  // before the ldp
    machine.__ss.__sp = address(&saved[0]);
    machine.__ss.__lr = address(apkrt_loader_back);  // the entry's return, into the stub
    auto after = apkrt::signals::after_loader_call(uc);
    CHECK(after.pc == address(probe_call_return));
    CHECK(after.sp == address(&saved[2]));
    CHECK(after.x[16] == 0x1616 && after.x[30] == address(probe_call_return));
    CHECK(after.x[0] == 7 && after.x[19] == 0x1919);
    for (const auto pc : {address(apkrt_loader_back) + 4, address(apkrt_loader_return)}) {
        machine.__ss.__pc = pc;  // after the ldp
        machine.__ss.__sp = address(&saved[2]);
        machine.__ss.__x[16] = 0x1616;
        machine.__ss.__lr = address(probe_call_return);
        after = apkrt::signals::after_loader_call(uc);
        CHECK(after.pc == address(probe_call_return));
        CHECK(after.sp == address(&saved[2]));
        CHECK(after.x[0] == 7 && after.x[16] == 0x1616);
    }
}

/// A guest stack with a syscall frame on it as syscall_entry.S leaves it for a syscall from
/// probe_site: room below for the signal frame a handler gets, the frame, the stub's two words.
struct alignas(16) FrameStack {
    std::uint8_t below[16 * 1024];
    apkrt_syscall_frame frame;
    std::uint64_t veneer[2];  // the stub's stp x16, x30
};

/// The frame of a read of descriptor 5 that a runtime signal interrupted in the host: x0 is -EINTR,
/// x[31] the first argument, which the dispatcher keeps there.
void interrupted_read(FrameStack &stack) {
    stack.frame = distinct_state(0x6000'0000, 0x0800'009f);
    stack.frame.x[0] = static_cast<std::uint64_t>(-eintr);
    stack.frame.x[3] = 0;
    stack.frame.x[8] = nr_read;
    stack.frame.x[31] = 5;
    stack.frame.x[30] = address(probe_stub_return);
    stack.veneer[0] = 0x1616;
    stack.veneer[1] = 0x3030;
}

/// A signal context stopped at `pc` in the exit, with `stack` as the exit has it there: sp at the
/// frame, or at the final ret the registers restored, the first argument in x16 (the exit's last
/// load) and sp above the frame.
void stopped_in_exit(ucontext_t &uc, _STRUCT_MCONTEXT64 &machine, FrameStack &stack, std::uint64_t pc) {
    machine = {};
    uc = {};
    uc.uc_mcontext = &machine;
    machine.__ss.__pc = pc;
    if (pc != address(apkrt_syscall_return)) {
        machine.__ss.__sp = address(&stack.frame);
        return;
    }
    const auto &frame = stack.frame;
    for (int index = 0; index < 29; ++index) machine.__ss.__x[index] = frame.x[index];
    machine.__ss.__x[16] = frame.x[31];
    machine.__ss.__fp = frame.x[29];
    machine.__ss.__lr = frame.x[30];
    machine.__ss.__sp = address(&stack.veneer[0]);
    machine.__ss.__cpsr = static_cast<std::uint32_t>(frame.nzcv);
    std::memcpy(machine.__ns.__v, frame.q, sizeof machine.__ns.__v);
    machine.__ns.__fpsr = static_cast<std::uint32_t>(frame.fpsr);
}

/// The words of the exit where a runtime signal finds the thread returned: after the clear, at the
/// final ret, and in the slow path before it sets in_runtime again.
std::vector<std::uint64_t> exit_window() {
    return {address(apkrt_syscall_returning), address(apkrt_syscall_return), address(apkrt_syscall_returned) - 4};
}

// ADR-0025, decision 3: a thread stopped anywhere in the exit's window gets the guest's state after
// the syscall, as the stub leaves it: x0 the result, x16 and x30 from the stub's words, sp above
// them, pc the word after the site.
TEST(a_thread_stopped_in_the_exit_counts_as_returned) {
    static FrameStack stack;
    for (const auto pc : exit_window()) {
        interrupted_read(stack);
        stack.frame.x[0] = 42;  // a result: nothing to restart
        ucontext_t uc;
        _STRUCT_MCONTEXT64 machine;
        stopped_in_exit(uc, machine, stack, pc);
        const auto after = apkrt::signals::after_syscall_exit(uc, 0);
        CHECK(after.pc == address(probe_after_site));
        CHECK(after.sp == address(&stack.veneer[2]));
        CHECK(after.x[0] == 42 && after.x[16] == 0x1616 && after.x[30] == 0x3030);
        CHECK(after.x[1] == stack.frame.x[1] && after.x[8] == nr_read && after.x[29] == stack.frame.x[29]);
        CHECK(after.q[31] == stack.frame.q[31] && after.nzcv == 0x6000'0000 && after.fpsr == 0x0800'009f);
    }
}

struct ExitView {
    int count = 0;
    bool entered[4] = {};
    std::uint64_t x0[4] = {};
    std::uint64_t pc[4] = {};
};

void exit_window_child(void *argument) {
    auto &view = *static_cast<ExitView *>(argument);
    const auto tid = static_cast<std::int32_t>(probe_syscall(nr_gettid, 0));
    struct sigaction installed {};
    ::sigaction(SIGURG, nullptr, &installed);
    static FrameStack stack;
    for (const auto pc : exit_window()) {
        apkrt::signals::LinuxSiginfo info{};
        info.signo = static_cast<std::int32_t>(sigusr1);
        apkrt::signals::send_to_thread(tid, static_cast<int>(sigusr1), info);  // own thread: pending, no nudge
        interrupted_read(stack);
        ucontext_t uc;
        _STRUCT_MCONTEXT64 machine;
        stopped_in_exit(uc, machine, stack, pc);
        siginfo_t host_info{};
        installed.sa_sigaction(SIGURG, &host_info, &uc);  // as if another thread's runtime signal came
        const int index = view.count++;
        view.entered[index] = machine.__ss.__pc == address(reinterpret_cast<const void *>(&on_signal));
        const auto frame_context = static_cast<std::uint64_t>(machine.__ss.__x[2]);  // the handler's ucontext
        view.x0[index] = *reinterpret_cast<const std::uint64_t *>(frame_context + linux_regs_offset);
        view.pc[index] = *reinterpret_cast<const std::uint64_t *>(frame_context + linux_pc_offset);
        apkrt::signals::current().mask.store(0);  // the handler's mask, which its rt_sigreturn would undo
    }
}

// ADR-0025: a runtime signal that finds the thread in the exit's window delivers under the same
// SA_RESTART rule as the exit's slow path: the handler's frame returns to the site with the first
// argument, so the read starts again. The real handler, on a guest thread, at every word of the window.
TEST(a_signal_in_the_exit_window_keeps_the_restart_rule) {
    static ExitView view;
    view = {};
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, exit_window_child, &view) > 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    CHECK(view.count == 3);
    for (int index = 0; index < view.count; ++index) {
        CHECK(view.entered[index]);
        if (view.x0[index] != 5 || view.pc[index] != address(probe_site)) {
            std::fprintf(stderr, "word %d of the window: the frame returns to pc %#llx with x0 %#llx\n", index,
                         static_cast<unsigned long long>(view.pc[index]), static_cast<unsigned long long>(view.x0[index]));
        }
        CHECK(view.x0[index] == 5);
        CHECK(view.pc[index] == address(probe_site));
    }
}

void bare_exit_child(void *argument) {
    auto &view = *static_cast<SpinView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    const std::uint64_t set = bit(sigusr1);
    CHECK(probe_syscall(nr_rt_sigprocmask, sig_block, reinterpret_cast<std::uint64_t>(&set), 0, 8) == 0);
    apkrt::signals::LinuxSiginfo info{};
    info.signo = static_cast<std::int32_t>(sigusr1);
    apkrt::signals::send_to_thread(static_cast<std::int32_t>(view.tid.load()), static_cast<int>(sigusr1), info);
    const int before = seen.count[sigusr1].load();
    CHECK(probe_syscall_without_veneer(nr_rt_sigprocmask, sig_unblock, reinterpret_cast<std::uint64_t>(&set), 0, 8) == 0);
    view.arrived = static_cast<int>(seen.count[sigusr1].load() == before + 1);
}

// A syscall that did not come through a veneer delivers at its end as well, on the state at its
// return address, like the one through a veneer.
TEST(a_signal_arrives_at_the_end_of_a_syscall_without_a_veneer) {
    static SpinView view;
    view.tid = 0;
    view.arrived = 0;
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, bare_exit_child, &view) > 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    CHECK(view.arrived.load() == 1);
}

void spin_after_plain_syscall_child(void *argument) {
    auto &view = *static_cast<SpinView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    spin_for_signal(view);
}

// ADR-0025, decision 2: the exit clears in_runtime, so guest code after a plain syscall gets a
// signal from another thread.
TEST(a_signal_reaches_guest_code_after_a_plain_syscall) {
    CHECK(signal_reaches_spinning_child(spin_after_plain_syscall_child));
}

// The labels mark where in_runtime changes (ADR-0025): the runtime signal's handler relies on each
// of them coming right after its store, and at the final ret on the first argument in x16.
TEST(the_flag_stores_sit_right_before_their_labels) {
    CHECK(apkrt_syscall_entered[-1] == 0x3900'412a);    // strb w10, [x9, #16]: set
    CHECK(apkrt_syscall_returning[-1] == 0x3900'401f);  // strb wzr, [x0, #16]: cleared
    CHECK(apkrt_syscall_returned[-1] == 0x3900'4001);   // strb w1, [x0, #16]: set again
    CHECK(apkrt_syscall_return[-2] == 0xf940'7ff0);     // ldr x16, [sp, #248]: x[31]
    CHECK(apkrt_syscall_return[-1] == 0x910c'43ff);     // add sp, sp, #784
}

struct LoaderView {
    std::atomic<long> tid{0};
    std::atomic<int> calling{0};
    std::atomic<int> returned{0};
    std::uint64_t result = ~std::uint64_t{0};
    int after = -1;  // SIGUSR1 handlers that ran by the first word after the call
};
apkrt_loader loader_table{};

void loader_child(void *argument) {
    auto &view = *static_cast<LoaderView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    struct {
        const char *fname;
        void *fbase;
        const char *sname;
        void *saddr;
    } info{};  // Dl_info on Android
    const int before = seen.count[sigusr1].load();
    view.calling = 1;
    view.result = probe_call(reinterpret_cast<const void *>(loader_table.dladdr), address(&info),
                             reinterpret_cast<std::uint64_t>(&info));
    view.after = seen.count[sigusr1].load() - before;
    view.returned = 1;
}

// ADR-0025, decisions 4 and 9: while ld-android.so's call into the loader runs, waiting for the
// loader's lock included, the thread is in the runtime: a signal waits for the call's return and
// arrives right there, on the caller's registers and pc. Guest callers wait for the lock in
// ld-android.so instead, in guest code (addition d, fixture dl-lock-test).
TEST(a_signal_waits_for_a_call_into_the_loader_to_return) {
    apkrt::dl::set_loader_functions(loader_table);
    static LoaderView view;
    view.tid = 0;
    view.calling = 0;
    view.returned = 0;
    seen.pcs_recorded = 0;
    const int before = seen.count[sigusr1].load();
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    int during = -1;
    {
        const std::lock_guard held(apkrt::dl::lock());  // the call waits for it, inside the loader
        CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, loader_child, &view) > 0);
        while (view.calling.load() == 0) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        seen.recording = true;
        CHECK(send(view.tid.load(), sigusr1) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        during = seen.count[sigusr1].load() - before;
        CHECK(view.returned.load() == 0);
    }
    join(&tid_word);
    seen.recording = false;
    ::munmap(stack, stack_size);
    CHECK(during == 0);
    CHECK(view.result == 0);  // no loaded library holds the address
    CHECK(view.after == 1);
    CHECK(seen.pcs_recorded.load() == 1);
    if (seen.pcs_recorded.load() == 1) CHECK(seen.pcs[0] == address(probe_call_return));
}

const char *program = nullptr;  // argv[0], to start the test again in a child process

constexpr std::uint64_t sigsegv = 11;

void on_segv(int, void *, void *) {
    static const char message[] = "the guest's SIGSEGV handler ran\n";
    static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
    ::_exit(42);
}

void loader_fault_child(void *) {
    probe_call(reinterpret_cast<const void *>(loader_table.dlsym), 0 /* RTLD_DEFAULT */, 1 /* no name */);
}

/// In the child process: a guest thread whose call into the loader faults.
int run_loader_fault() {
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{reinterpret_cast<std::uint64_t>(&on_segv), sa_siginfo | sa_restorer,
             reinterpret_cast<std::uint64_t>(&probe_sigreturn), 0};
    if (probe_syscall(nr_rt_sigaction, sigsegv, reinterpret_cast<std::uint64_t>(&action), 0, 8) != 0) return 2;
    apkrt::dl::set_loader_functions(loader_table);
    void *stack = map_stack();
    int tid_word = 0;
    if (probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, loader_fault_child, nullptr) <= 0) return 2;
    join(&tid_word);
    return 3;  // the fault should have ended the process
}

// ADR-0025, decision 4: a fault inside the loader is the runtime's own, even when guest code called it:
// the process dies as with any fault of the runtime, and the guest's SIGSEGV handler does not run.
TEST(a_fault_in_the_loader_is_the_runtime_s_own) {
    char mode[] = "--loader-fault";
    char *arguments[] = {const_cast<char *>(program), mode, nullptr};
    // The child's stderr gets the report of the fault; the test only looks at how it ended.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t child = 0;
    CHECK(::posix_spawn(&child, program, &actions, nullptr, arguments, nullptr) == 0);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    const bool guest_handler = WIFEXITED(status) && WEXITSTATUS(status) == 42;  // on_segv's _exit
    if (guest_handler) std::fprintf(stderr, "the fault in the loader went to the guest's handler\n");
    CHECK(!guest_handler);
    CHECK(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 3));
}

constexpr std::uint64_t nr_getpid = 172, nr_exit = 93;

std::uint64_t guest_sum(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) {
    return a0 + a1 + a2;
}

struct CallView {
    std::atomic<long> tid{0};
    std::atomic<int> spinning{0};
    int in_runtime_inside = -1;        // what the guest function found
    int in_runtime_after_syscall = -1;  // after its own syscall
    int arrived_inside = -1;           // the signal from the test thread came while it ran
    std::uint64_t result = 0;
    int in_runtime_after = -1;         // what the runtime found after the call
};
CallView call_view;

std::uint64_t guest_function(std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) {
    auto &view = call_view;
    view.in_runtime_inside = static_cast<int>(apkrt::signals::current().in_runtime.load());
    // First without a syscall, whose exit would clear the flag anyway.
    const int before = seen.count[sigusr1].load();
    view.spinning = 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (seen.count[sigusr1].load() == before && std::chrono::steady_clock::now() < deadline) {
    }
    view.arrived_inside = static_cast<int>(seen.count[sigusr1].load() != before);
    CHECK(probe_syscall(nr_getpid, 0) == ::getpid());
    view.in_runtime_after_syscall = static_cast<int>(apkrt::signals::current().in_runtime.load());
    return a0 + a1 + a2;
}

void call_child(void *) {
    auto &view = call_view;
    view.tid = probe_syscall(nr_gettid, 0);
    auto &state = apkrt::signals::current();
    state.in_runtime = true;  // as in the runtime's own code, where it calls guest code
    view.result = apkrt_call_guest(reinterpret_cast<std::uint64_t>(&guest_function), 1, 20, 300, 0, 0, 0, 0, 0);
    view.in_runtime_after = static_cast<int>(state.in_runtime.load());
    state.in_runtime = false;  // back in the test's code, guest code again
}

// ADR-0025, decision 5: the runtime calls guest code through apkrt_call_guest, which clears in_runtime
// for the call and sets it again after, also when the guest code's own syscall cleared it. A signal
// from another thread reaches the guest code while it runs.
TEST(the_runtime_calls_guest_code_as_guest_code) {
    auto &view = call_view;
    view.tid = 0;
    view.spinning = 0;
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, call_child, nullptr) > 0);
    while (view.spinning.load() == 0) std::this_thread::yield();
    CHECK(send(view.tid.load(), sigusr1) == 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    CHECK(view.in_runtime_inside == 0);
    CHECK(view.in_runtime_after_syscall == 0);
    CHECK(view.arrived_inside == 1);
    CHECK(view.result == 321);
    CHECK(view.in_runtime_after == 1);
}

struct StartView {
    int in_runtime_at_start = -1;
    int handled_at_start = -1;
};
int handled_before_start;
StartView start_view;
void *program_stack = nullptr;

[[noreturn]] void program_entry() {
    start_view.handled_at_start = seen.count[sigusr1].load() - handled_before_start;
    start_view.in_runtime_at_start = static_cast<int>(apkrt::signals::current().in_runtime.load());
    probe_syscall(nr_exit, 0);
    __builtin_trap();  // exit does not return
}

void start_child(void *) {
    const auto tid = static_cast<std::int32_t>(probe_syscall(nr_gettid, 0));
    apkrt::signals::current().in_runtime = true;  // the program thread runs the runtime until then
    // A signal that came while the runtime set the program up, from its constructors, say.
    handled_before_start = seen.count[sigusr1].load();
    apkrt::signals::LinuxSiginfo info{};
    info.signo = static_cast<std::int32_t>(sigusr1);
    apkrt::signals::send_to_thread(tid, static_cast<int>(sigusr1), info);  // own thread: pending
    apkrt::signals::enter_guest(reinterpret_cast<std::uint64_t>(&program_entry),
                                reinterpret_cast<std::uint64_t>(top_of(program_stack)) - 64);
}

// ADR-0025, decision 6: the program starts as guest code, and what became pending while the runtime
// worked arrives before its first instruction.
TEST(the_program_starts_as_guest_code) {
    program_stack = map_stack();
    void *stack = map_stack();
    CHECK(stack != nullptr && program_stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, start_child, nullptr) > 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    ::munmap(program_stack, stack_size);
    CHECK(start_view.in_runtime_at_start == 0);
    CHECK(start_view.handled_at_start == 1);
}

struct ConstructorView {
    int in_runtime_inside = -1;
    int arguments_seen = 0;
    int in_runtime_after = -1;
};
ConstructorView constructor_view;

void constructor(int argc, char **argv, char **envp) {
    constructor_view.in_runtime_inside = static_cast<int>(apkrt::signals::current().in_runtime.load());
    constructor_view.arguments_seen = static_cast<int>(argc == 1 && argv != nullptr && envp != nullptr);
    CHECK(probe_syscall(nr_getpid, 0) == ::getpid());  // its exit clears in_runtime
}

void constructors_child(void *) {
    static const std::uint64_t init_array[] = {reinterpret_cast<std::uint64_t>(&constructor)};
    static apkrt::linker::Object object;
    object.name = "libconstructor.so";
    object.dynamic.init_array = init_array;
    const apkrt::linker::Object *objects[] = {&object};
    static char name[] = "program";
    static char *argv[] = {name, nullptr};
    static char *envp[] = {nullptr};
    auto &state = apkrt::signals::current();
    state.in_runtime = true;  // as in dlopen, which runs them
    apkrt::linker::run_constructors(objects, 1, argv, envp, false);
    constructor_view.in_runtime_after = static_cast<int>(state.in_runtime.load());
    state.in_runtime = false;
}

// ADR-0025, decision 5, at a call site: the loader runs a library's constructors as guest code, and
// is back in the runtime once they return, although the constructor made a syscall.
TEST(constructors_run_as_guest_code) {
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, constructors_child, nullptr) > 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    CHECK(constructor_view.in_runtime_inside == 0);
    CHECK(constructor_view.arguments_seen == 1);
    CHECK(constructor_view.in_runtime_after == 1);
}

int handled_before_call = 0;
int handled_at_first_word = -1;

std::uint64_t count_on_entry(std::uint64_t, std::uint64_t, std::uint64_t) {
    handled_at_first_word = seen.count[sigusr1].load() - handled_before_call;
    return 0;
}

void pending_call_child(void *) {
    const auto tid = static_cast<std::int32_t>(probe_syscall(nr_gettid, 0));
    auto &state = apkrt::signals::current();
    state.in_runtime = true;  // the runtime works, a signal comes, then it calls guest code
    handled_before_call = seen.count[sigusr1].load();
    apkrt::signals::LinuxSiginfo info{};
    info.signo = static_cast<std::int32_t>(sigusr1);
    apkrt::signals::send_to_thread(tid, static_cast<int>(sigusr1), info);  // own thread: pending
    apkrt_call_guest(reinterpret_cast<std::uint64_t>(&count_on_entry), 0, 0, 0, 0, 0, 0, 0, 0);
    state.in_runtime = false;
}

// ADR-0025, decision 5: a signal that became pending while the runtime worked arrives before the first
// instruction of the guest code the runtime calls, which may never make a syscall.
TEST(a_pending_signal_arrives_when_the_runtime_calls_guest_code) {
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, pending_call_child, nullptr) > 0);
    join(&tid_word);
    ::munmap(stack, stack_size);
    CHECK(handled_at_first_word == 1);
}

// On a host thread (this test's own), apkrt_call_guest is a plain call.
TEST(a_host_thread_calls_guest_code_directly) {
    CHECK(apkrt_call_guest(reinterpret_cast<std::uint64_t>(&guest_sum), 4, 50, 600, 0, 0, 0, 0, 0) == 654);
}

}  // namespace

int main(int argc, char **argv) {
    program = argv[0];
    if (argc > 1 && std::strcmp(argv[1], "--loader-fault") == 0) return run_loader_fault();
    // The test's own thread is the program's first thread: the process lives as long as it does.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    install_handlers();
    return apk::testing::run_all();
}
