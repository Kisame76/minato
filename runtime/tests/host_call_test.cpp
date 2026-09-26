// Test 3 of ADR-0030: the entry for calls from guest code into host code (decision 1, amendments A1 to
// A3), the private syscall "bind host table" with graphics/host's providers (decision 3), and
// apkrt_call_guest with eight arguments (decision 5). The guest threads are clone children that call
// host functions the way a front lib's stubs do (host_call_probe.S); the guest signals come from the
// test's own thread, like from another thread of the program.
#include "signals.h"
#include "syscall_frame.h"
#include "threads.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"

#include "check.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using Word = std::uint64_t;

extern "C" {
// probe_clone loads x19-x28 and v8-v15 from probe_marks and stores the child's into
// probe_child_registers (threads_test.cpp checks them; here they only have to exist).
struct ProbeRegisters {
    Word x[10];
    __uint128_t q[8];
    Word sp;
};
ProbeRegisters probe_marks;
ProbeRegisters probe_child_registers;

long probe_clone(Word flags, void *child_stack, int *parent_tid, void *tls, int *child_tid, void (*function)(void *),
                 void *argument);
long probe_syscall(Word number, Word a0, Word a1 = 0, Word a2 = 0, Word a3 = 0);
void probe_sigreturn();

// host_call_probe.S
Word probe_table[4];
struct ProbeResults {
    Word x[2];
    __uint128_t q[4];
};
ProbeResults probe_results;
Word probe_stub_1(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7, Word s0, Word s1);
Word probe_stub_2(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7, Word s0, Word s1);
Word probe_stub_3(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7, double d0, double d1,
                  double d2, double d3, double d4, double d5, double d6, double d7);
Word probe_host_call(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7);
extern const std::uint32_t probe_host_call_return[];
void probe_host_call_state(const apkrt_syscall_frame *before, apkrt_syscall_frame *after);
void probe_host_clobber();
}

namespace {

using apkrt::signals::current;

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, asm-generic/signal.h, errno.h.
constexpr Word pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr Word nr_futex = 98, nr_tgkill = 131, nr_rt_sigaction = 134, nr_gettid = 178, nr_read = 63,
               nr_sigaltstack = 132;
constexpr Word sigusr1 = 10, sigsegv = 11, sigusr2 = 12;
constexpr Word sa_siginfo = 0x4, sa_restorer = 0x0400'0000, sa_restart = 0x1000'0000, sa_onstack = 0x0800'0000;
constexpr long enoent = 2, eagain = 11, einval = 22;
// The Linux ucontext on arm64: struct sigcontext's regs[31] after fault_address, then sp and pc.
constexpr std::size_t linux_regs_offset = 184, linux_sp_offset = 432, linux_pc_offset = 440;

constexpr Word bit(Word signal) {
    return Word{1} << (signal - 1);
}

Word address(const void *pointer) {
    return reinterpret_cast<Word>(pointer);
}

template <typename Function>
Word address_of(Function *function) {
    return reinterpret_cast<Word>(function);
}

constexpr std::size_t stack_size = 256 * 1024;

void *map_memory(std::size_t size) {
    void *memory = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(memory != MAP_FAILED);
    return memory;
}

/// A clone child, a guest thread of its own, on a stack the test maps.
struct Child {
    void *stack = nullptr;
    std::size_t size = 0;
    int tid_word = 0;

    void start(void (*function)(void *), void *argument = nullptr) {
        start_on(map_memory(stack_size), stack_size, function, argument);
    }

    /// On the lowest stack_size bytes of `memory`, a mapping of `mapped` bytes that join() unmaps.
    void start_on(void *memory, std::size_t mapped, void (*function)(void *), void *argument) {
        stack = memory;
        size = mapped;
        tid_word = 0;
        CHECK(probe_clone(pthread_flags, static_cast<char *>(stack) + stack_size, &tid_word, nullptr, &tid_word,
                          function, argument) > 0);
    }

    /// pthread_join's wait: until the kernel clears the thread ID word at the thread's exit.
    void join() {
        for (int value; (value = __atomic_load_n(&tid_word, __ATOMIC_ACQUIRE)) != 0;) {
            const long result = probe_syscall(nr_futex, address(&tid_word), 0 /* FUTEX_WAIT */,
                                              static_cast<Word>(value), 0);
            CHECK(result == 0 || result == -eagain);
        }
        ::munmap(stack, size);
    }
};

void run_on_guest_thread(void (*function)(void *), void *argument = nullptr) {
    Child child;
    child.start(function, argument);
    child.join();
}

/// The table the probe's stubs read: slot 0 the runtime's entry, then the host functions.
void set_table(Word slot1, Word slot2 = 0, Word slot3 = 0) {
    probe_table[0] = address_of(&apkrt_host_call);
    probe_table[1] = slot1;
    probe_table[2] = slot2;
    probe_table[3] = slot3;
}

/// What the guest's SIGUSR1 handler found in its ucontext, the last time it ran.
struct Seen {
    std::atomic<int> count{0};
    Word x0 = 0, x19 = 0, x29 = 0, sp = 0, pc = 0;
};
Seen seen;

Word linux_word(const void *ucontext, std::size_t offset) {
    Word value;
    std::memcpy(&value, static_cast<const char *>(ucontext) + offset, sizeof value);
    return value;
}

void on_signal(int, void *, void *ucontext) {
    seen.x0 = linux_word(ucontext, linux_regs_offset);
    seen.x19 = linux_word(ucontext, linux_regs_offset + 19 * 8);
    seen.x29 = linux_word(ucontext, linux_regs_offset + 29 * 8);
    seen.sp = linux_word(ucontext, linux_sp_offset);
    seen.pc = linux_word(ucontext, linux_pc_offset);
    seen.count.fetch_add(1);
}

/// SIGUSR1 goes to on_signal with SA_RESTART, which a host call's exit must not apply (A1).
void install_handler() {
    struct {
        Word handler, flags, restorer, mask;
    } action{address_of(&on_signal), sa_siginfo | sa_restorer | sa_restart, address_of(&probe_sigreturn), bit(sigusr1)};
    CHECK(probe_syscall(nr_rt_sigaction, sigusr1, address(&action), 0, 8) == 0);
}

long send(long tid, Word signal) {
    return probe_syscall(nr_tgkill, static_cast<Word>(::getpid()), static_cast<Word>(tid), signal);
}

long own_tid() {
    return probe_syscall(nr_gettid, 0);
}

/// 0 when a nanosleep of `nanoseconds` ran to its end, else its errno.
int nap(long nanoseconds) {
    const timespec time{0, nanoseconds};
    return ::nanosleep(&time, nullptr) == 0 ? 0 : errno;
}

void wait_until(const std::atomic<int> &flag) {
    while (flag.load() == 0) std::this_thread::yield();
}

int side() {
    return current().in_runtime.load();
}

int depth() {
    return static_cast<int>(current().host_calls);
}

// --- The entry on host and guest threads ---------------------------------------------------------

struct PlainView {
    int guest = -1;
    int side = -1;
};
PlainView plain_view;

Word host_weighted_sum(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7) {
    plain_view.guest = apkrt_is_guest_thread();
    plain_view.side = side();
    return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7;
}

// Decision 1: on a host thread the entry is a plain tail call.
TEST(on_a_host_thread_a_host_call_is_a_plain_call) {
    set_table(address_of(&host_weighted_sum));
    const int before = side();
    CHECK(probe_stub_1(1, 2, 3, 4, 5, 6, 7, 8, 0, 0) == 204);
    CHECK(plain_view.guest == 0);
    CHECK(plain_view.side == before);  // the host thread's own state stays as it is
    CHECK(side() == before);
    CHECK(depth() == 0);
}

Word guest_weighted_sum(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7) {
    return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7;
}

// Decision 5: on a host thread apkrt_call_guest is a plain call, the eighth argument included.
TEST(on_a_host_thread_apkrt_call_guest_is_a_plain_call_with_eight_arguments) {
    CHECK(apkrt_call_guest(address_of(&guest_weighted_sum), 1, 2, 3, 4, 5, 6, 7, 8) == 204);
}

int guest_thread_seen = -1;

// Decision 5: graphics/host asks before it calls an app's callback.
TEST(apkrt_is_guest_thread_tells_guest_threads_from_host_threads) {
    CHECK(apkrt_is_guest_thread() == 0);
    run_on_guest_thread([](void *) { guest_thread_seen = apkrt_is_guest_thread(); });
    CHECK(guest_thread_seen == 1);
}

struct SideView {
    int during = -1;
    int depth_during = -1;
    int after = -1;
    int depth_after = -1;
    Word result = 0;
};
SideView side_view;

Word host_sides(Word a0, Word a1) {
    side_view.during = side();
    side_view.depth_during = depth();
    return a0 * 10 + a1;
}

// Decision 1 and A2: the host function runs as host code, one call deep; guest code follows its return.
TEST(the_host_function_runs_as_host_code_and_guest_code_follows) {
    set_table(address_of(&host_sides));
    side_view = {};
    run_on_guest_thread([](void *) {
        side_view.result = probe_stub_1(4, 2, 0, 0, 0, 0, 0, 0, 0, 0);
        side_view.after = side();
        side_view.depth_after = depth();
    });
    CHECK(side_view.result == 42);
    CHECK(side_view.during == apkrt::signals::host_code);
    CHECK(side_view.depth_during == 1);
    CHECK(side_view.after == apkrt::signals::guest_code);
    CHECK(side_view.depth_after == 0);
}

/// Every register different, as syscall_entry_test.c chooses them.
apkrt_syscall_frame distinct_state() {
    apkrt_syscall_frame state{};
    for (int index = 0; index < 32; ++index) {
        state.x[index] = 0x0101'0101'0101'0101ull * static_cast<Word>(index + 1) ^ 0x8000'0000'0000'0000ull;
        state.q[index] = (static_cast<__uint128_t>(0xa5a5'a5a5'a5a5'a5a5ull + static_cast<Word>(index)) << 64) |
                         static_cast<Word>(index * 977 + 1);
    }
    return state;
}

apkrt_syscall_frame state_before;
apkrt_syscall_frame state_after;

// Decision 1: x19-x29, d8-d15 and the stack pointer are kept, as both ABIs keep them across a call, and
// the results in x0, x1 and v0-v3 come back from the host function.
TEST(a_host_call_keeps_what_both_abis_keep_and_brings_back_the_results) {
    set_table(address_of(&probe_host_clobber));
    probe_results.x[0] = 0x0123'4567'89ab'cdefull;
    probe_results.x[1] = ~probe_results.x[0];
    for (int index = 0; index < 4; ++index) {
        probe_results.q[index] = (static_cast<__uint128_t>(0x7e57'0000'0000'0000ull + static_cast<Word>(index)) << 64) |
                                 static_cast<Word>(0xc0de + index);
    }
    state_before = distinct_state();
    state_after = {};
    run_on_guest_thread([](void *) { probe_host_call_state(&state_before, &state_after); });
    CHECK(state_after.x[0] == probe_results.x[0] && state_after.x[1] == probe_results.x[1]);
    for (int index = 0; index < 4; ++index) CHECK(state_after.q[index] == probe_results.q[index]);
    for (int index = 19; index < 30; ++index) {
        if (state_after.x[index] != state_before.x[index]) std::fprintf(stderr, "x%d changed\n", index);
        CHECK(state_after.x[index] == state_before.x[index]);
    }
    for (int index = 8; index < 16; ++index) {
        CHECK(static_cast<Word>(state_after.q[index]) == static_cast<Word>(state_before.q[index]));
    }
    CHECK(state_after.x[31] == state_after.x[30]);  // the stack pointer
}

struct StackView {
    Word s0 = 0, s1 = 0, a7 = 0;
};
StackView stack_view;

Word host_ten(Word, Word, Word, Word, Word, Word, Word, Word a7, Word s0, Word s1) {
    stack_view = {s0, s1, a7};
    return 0;
}

// Decision 1: the entry leaves the stack pointer as it is, so stack arguments arrive where the stub's
// caller put them.
TEST(stack_arguments_arrive_in_place) {
    set_table(address_of(&host_ten));
    stack_view = {};
    run_on_guest_thread([](void *) { probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0x77, 0x5151, 0x5252); });
    CHECK(stack_view.a7 == 0x77);
    CHECK(stack_view.s0 == 0x5151);
    CHECK(stack_view.s1 == 0x5252);
}

// --- Guest signals and the runtime signal ----------------------------------------------------------

struct WaitView {
    std::atomic<long> tid{0};
    std::atomic<int> inside{0};
    int slept = -1;
    int handled_inside = -1;
    Word result = 0;
};
WaitView wait_view;

Word host_sleep() {
    const int before = seen.count.load();
    wait_view.inside = 1;
    wait_view.slept = nap(200'000'000);
    wait_view.handled_inside = seen.count.load() - before;
    return 0x5eed;
}

void wait_child(void *) {
    wait_view.tid = own_tid();
    wait_view.result = probe_host_call(0, 0, 0, 0, 0, 0, 0, 0);
}

// Decision 1: while a thread is inside a host call it gets no runtime signal, so the host's own sleep
// runs to its end; the guest signal sent meanwhile arrives right after the call returns, on the
// caller's registers and pc, with the call's result.
TEST(a_guest_signal_waits_for_the_host_call_and_arrives_right_after_it) {
    set_table(address_of(&host_sleep));
    wait_view.tid = 0;
    wait_view.inside = 0;
    const int before = seen.count.load();
    Child child;
    child.start(wait_child);
    wait_until(wait_view.inside);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(send(wait_view.tid.load(), sigusr1) == 0);
    child.join();
    CHECK(wait_view.slept == 0);
    CHECK(wait_view.handled_inside == 0);
    CHECK(seen.count.load() - before == 1);
    CHECK(wait_view.result == 0x5eed);
    CHECK(seen.pc == address(probe_host_call_return));
    CHECK(seen.x0 == 0x5eed);
    CHECK(seen.sp == seen.x29);  // probe_host_call calls with x29 = sp
}

std::uint64_t label(const std::uint32_t *word) {
    return address(word);
}

// Decision 1, like ADR-0025, decision 3: by its pc and in_runtime, a runtime signal tells the entry
// before its store (the signal waits for the exit), host code (it waits), and the exit after its
// clear (counts as returned) apart.
TEST(a_runtime_signal_knows_the_edges_of_a_host_call) {
    using apkrt::signals::Stopped;
    using apkrt::signals::where_stopped;
    const auto entry = address_of(&apkrt_host_call);
    CHECK(where_stopped(entry, false) == Stopped::in_runtime);
    CHECK(where_stopped(label(apkrt_host_call_entered) - 4, false) == Stopped::in_runtime);  // the store
    CHECK(where_stopped(label(apkrt_host_call_entered), true) == Stopped::in_runtime);
    CHECK(where_stopped(label(apkrt_host_call_returning) - 4, true) == Stopped::in_runtime);  // the clear
    for (auto pc = label(apkrt_host_call_returning); pc < label(apkrt_host_call_returned); pc += 4) {
        CHECK(where_stopped(pc, false) == Stopped::host_returning);
    }
    CHECK(where_stopped(label(apkrt_host_call_return), false) == Stopped::host_returning);
    CHECK(where_stopped(label(apkrt_host_call_returned), true) == Stopped::in_runtime);
}

// Decision 1: the labels mark where in_runtime changes, and the handler relies on each coming right after
// its store.
TEST(the_host_call_s_flag_stores_sit_right_before_their_labels) {
    CHECK(apkrt_host_call_entered[-1] == 0x089f'fd6a);    // stlrb w10, [x11]: host code
    CHECK(apkrt_host_call_returning[-1] == 0x089f'fd7f);  // stlrb wzr, [x11]: guest code
    CHECK(apkrt_host_call_returned[-1] == 0x3900'412a);   // strb w10, [x9, #16]: runtime code
    CHECK(apkrt_host_call_return[0] == 0xd65f'03c0);      // ret
}

struct WindowView {
    int count = 0;
    bool entered[16] = {};
    Word x0[16] = {};
    Word pc[16] = {};
    Word sp[16] = {};
};
WindowView window_view;

alignas(16) std::uint8_t window_stack[16 * 1024];

void window_child(void *) {
    const auto tid = static_cast<std::int32_t>(own_tid());
    struct sigaction installed {};
    ::sigaction(SIGURG, nullptr, &installed);
    const Word top = address(window_stack + sizeof window_stack);
    for (auto pc = label(apkrt_host_call_returning); pc < label(apkrt_host_call_returned) && window_view.count < 16;
         pc += 4) {
        apkrt::signals::LinuxSiginfo info{};
        info.signo = static_cast<std::int32_t>(sigusr1);
        apkrt::signals::send_to_thread(tid, static_cast<int>(sigusr1), info);  // own thread: pending, no nudge
        current().in_runtime = apkrt::signals::guest_code;                    // as the exit leaves it
        _STRUCT_MCONTEXT64 machine{};
        ucontext_t uc{};
        uc.uc_mcontext = &machine;
        machine.__ss.__pc = pc;
        machine.__ss.__sp = top;
        machine.__ss.__lr = address(probe_host_call_return);  // the popped return address
        machine.__ss.__x[0] = static_cast<Word>(-4);            // -EINTR, or VK_ERROR_DEVICE_LOST
        machine.__ss.__x[8] = nr_read;                          // no syscall number, but it looks like one
        siginfo_t host_info{};
        installed.sa_sigaction(SIGURG, &host_info, &uc);  // as if another thread's runtime signal came
        const int index = window_view.count++;
        window_view.entered[index] = machine.__ss.__pc == address_of(&on_signal);
        const auto frame_context = static_cast<Word>(machine.__ss.__x[2]);  // the handler's ucontext
        window_view.x0[index] = linux_word(reinterpret_cast<const void *>(frame_context), linux_regs_offset);
        window_view.pc[index] = linux_word(reinterpret_cast<const void *>(frame_context), linux_pc_offset);
        window_view.sp[index] = linux_word(reinterpret_cast<const void *>(frame_context), linux_sp_offset);
        current().mask.store(0);  // the handler's mask, which its rt_sigreturn would undo
    }
}

// A1 and decision 1: a runtime signal that finds the thread anywhere in the exit's window delivers on the
// state after the call, with its result and pc = the popped return address, and without the SA_RESTART
// rule: a host call is never made again. The real handler, on a guest thread, at every word.
TEST(a_signal_in_the_exit_window_arrives_after_the_call_and_never_restarts_it) {
    window_view = {};
    run_on_guest_thread(window_child);
    const auto words = static_cast<int>((label(apkrt_host_call_returned) - label(apkrt_host_call_returning)) / 4);
    CHECK(words >= 2);  // at least the check and the ret
    CHECK(window_view.count == words);
    for (int index = 0; index < window_view.count; ++index) {
        CHECK(window_view.entered[index]);
        if (window_view.pc[index] != address(probe_host_call_return) || window_view.x0[index] != static_cast<Word>(-4)) {
            std::fprintf(stderr, "word %d of the window: the frame returns to pc %#llx with x0 %#llx\n", index,
                         static_cast<unsigned long long>(window_view.pc[index]),
                         static_cast<unsigned long long>(window_view.x0[index]));
        }
        CHECK(window_view.pc[index] == address(probe_host_call_return));
        CHECK(window_view.x0[index] == static_cast<Word>(-4));
        CHECK(window_view.sp[index] == address(window_stack + sizeof window_stack));
    }
}

// --- Nesting ---------------------------------------------------------------------------------------

struct NestView {
    int host_side = -1, guest_side = -1, inner_side = -1, back_in_host = -1, after = -1;
    int depth_inner = -1, depth_back = -1, depth_after = -1;
    Word guest_arguments[8] = {};
    Word result = 0;
};
NestView nest_view;

Word inner_host(Word a0, Word, Word, Word, Word, Word, Word, Word a7) {
    nest_view.inner_side = side();
    nest_view.depth_inner = depth();
    return a0 + a7;
}

Word guest_callback(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7) {
    nest_view.guest_side = side();
    const Word arguments[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    std::memcpy(nest_view.guest_arguments, arguments, sizeof arguments);
    return probe_stub_2(a0, a1, a2, a3, a4, a5, a6, a7, 0, 0) + 1;  // host code again, one call deeper
}

Word outer_host(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7) {
    nest_view.host_side = side();
    const Word result = apkrt_call_guest(address_of(&guest_callback), a0, a1, a2, a3, a4, a5, a6, a7);
    nest_view.back_in_host = side();
    nest_view.depth_back = depth();
    return result * 2;
}

// Decisions 1 and 5, A2: host code calls a guest callback with eight arguments, which calls host code
// again; apkrt_call_guest gives the callback guest code and host code back what it found.
TEST(host_and_guest_code_nest_with_eight_arguments) {
    set_table(address_of(&outer_host), address_of(&inner_host));
    nest_view = {};
    run_on_guest_thread([](void *) {
        nest_view.result = probe_stub_1(1, 2, 3, 4, 5, 6, 7, 8, 0, 0);
        nest_view.after = side();
        nest_view.depth_after = depth();
    });
    for (int index = 0; index < 8; ++index) CHECK(nest_view.guest_arguments[index] == static_cast<Word>(index + 1));
    CHECK(nest_view.result == 20);  // ((1 + 8) + 1) * 2
    CHECK(nest_view.host_side == apkrt::signals::host_code);
    CHECK(nest_view.guest_side == apkrt::signals::guest_code);
    CHECK(nest_view.inner_side == apkrt::signals::host_code);
    CHECK(nest_view.depth_inner == 2);
    CHECK(nest_view.back_in_host == apkrt::signals::host_code);
    CHECK(nest_view.depth_back == 1);
    CHECK(nest_view.after == apkrt::signals::guest_code);
    CHECK(nest_view.depth_after == 0);
}

struct EscapeView {
    int escapes = 0;
    int returned_normally = 0;
    int side_after_escapes = -1;
    int depth_inside_next = -1;
    int depth_after = -1;
};
EscapeView escape_view;
apkrt_host_context escape_point;

Word guest_escape() {
    apkrt_return_to_host(&escape_point);  // a longjmp out of the callback, past the host frames
}

Word host_calling_back() {
    apkrt_call_guest(address_of(&guest_escape), 0, 0, 0, 0, 0, 0, 0, 0);
    return 0;
}

Word host_depth() {
    return static_cast<Word>(depth());
}

void escape_child(void *) {
    // More rounds than the entry's stack holds: each leaves its call's entry behind.
    while (escape_view.escapes < 20) {
        if (apkrt_save_host(&escape_point) == 0) {
            probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            escape_view.returned_normally = 1;
            break;
        }
        ++escape_view.escapes;
    }
    escape_view.side_after_escapes = side();
    escape_view.depth_inside_next = static_cast<int>(probe_stub_2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    escape_view.depth_after = depth();
}

// Decision 1: an entry whose recorded stack pointer lies at or below the current one is stale, left
// behind by a longjmp out of a callback, and the next call drops it.
TEST(a_longjmp_out_of_a_nested_callback_leaves_nothing_stale) {
    set_table(address_of(&host_calling_back), address_of(&host_depth));
    escape_view = {};
    run_on_guest_thread(escape_child);
    CHECK(escape_view.escapes == 20);
    CHECK(escape_view.returned_normally == 0);
    CHECK(escape_view.side_after_escapes == apkrt::signals::guest_code);
    CHECK(escape_view.depth_inside_next == 1);
    CHECK(escape_view.depth_after == 0);
}

struct InnerEscapeView {
    int jumped = 0;
    int depth_back_in_outer = -1;
    int depth_after = -1;
    Word result = 0;
};
InnerEscapeView inner_escape_view;
apkrt_host_context inner_escape_point;

Word guest_inner_escape() {
    apkrt_return_to_host(&inner_escape_point);
}

Word host_inner_calling_back() {
    apkrt_call_guest(address_of(&guest_inner_escape), 0, 0, 0, 0, 0, 0, 0, 0);
    return 0;
}

Word guest_outer_callback() {
    if (apkrt_save_host(&inner_escape_point) == 0) {
        probe_stub_2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    } else {
        inner_escape_view.jumped = 1;
    }
    return 7;
}

Word host_outer_calling_back() {
    const Word result = apkrt_call_guest(address_of(&guest_outer_callback), 0, 0, 0, 0, 0, 0, 0, 0);
    inner_escape_view.depth_back_in_outer = depth();  // the inner call's entry is still there
    return result + 1;
}

// Decision 1: a longjmp from an inner callback into an outer one leaves the inner call's entry above the
// outer call's; the outer call's exit drops it, as made below its own stack pointer, and returns.
TEST(a_longjmp_into_an_outer_callback_leaves_the_outer_call_intact) {
    set_table(address_of(&host_outer_calling_back), address_of(&host_inner_calling_back));
    inner_escape_view = {};
    run_on_guest_thread([](void *) {
        inner_escape_view.result = probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        inner_escape_view.depth_after = depth();
    });
    CHECK(inner_escape_view.jumped == 1);
    CHECK(inner_escape_view.depth_back_in_outer == 2);
    CHECK(inner_escape_view.result == 8);
    CHECK(inner_escape_view.depth_after == 0);
}

struct OnstackView {
    int handled = 0;
    int depth_inside = -1;
    int depth_back = -1;
    int depth_after = -1;
    Word result = 0;
    Word handler_sp = 0;
    Word call_sp = 0;
};
OnstackView onstack_view;
constexpr std::size_t alternate_size = 64 * 1024;

void on_signal_onstack(int, void *, void *) {
    int local = 0;
    onstack_view.handler_sp = address(&local);
    onstack_view.depth_inside = static_cast<int>(probe_stub_2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    ++onstack_view.handled;
}

Word guest_raising() {
    int local = 0;
    onstack_view.call_sp = address(&local);
    send(own_tid(), sigusr2);  // own thread: its handler runs at the end of this syscall, on the alternate stack
    return 5;
}

Word host_raising_through_callback() {
    const Word result = apkrt_call_guest(address_of(&guest_raising), 0, 0, 0, 0, 0, 0, 0, 0);
    onstack_view.depth_back = depth();
    return result;
}

struct LinuxStackT {
    Word sp;
    std::int32_t flags;
    std::int32_t padding;
    Word size;
};

void onstack_child(void *alternate) {
    const LinuxStackT stack{address(alternate), 0, 0, alternate_size};
    CHECK(probe_syscall(nr_sigaltstack, address(&stack), 0) == 0);
    onstack_view.result = probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    onstack_view.depth_after = depth();
    const LinuxStackT off{0, 2 /* SS_DISABLE */, 0, 0};
    CHECK(probe_syscall(nr_sigaltstack, address(&off), 0) == 0);
}

// Decision 1: a guest signal handler that runs on an alternate stack, above the thread's own, while a
// callback of a host call runs, makes a host call of its own: the running call's entry is not left
// behind, although it was made at a lower stack pointer.
TEST(a_handler_on_an_alternate_stack_above_the_thread_s_own_makes_host_calls) {
    struct {
        Word handler, flags, restorer, mask;
    } action{address_of(&on_signal_onstack), sa_siginfo | sa_restorer | sa_onstack, address_of(&probe_sigreturn), 0};
    CHECK(probe_syscall(nr_rt_sigaction, sigusr2, address(&action), 0, 8) == 0);
    set_table(address_of(&host_raising_through_callback), address_of(&host_depth));
    onstack_view = {};
    // One mapping: the child's stack, and its alternate stack right above it.
    void *memory = map_memory(stack_size + alternate_size);
    Child child;
    child.start_on(memory, stack_size + alternate_size, onstack_child, static_cast<char *>(memory) + stack_size);
    child.join();
    CHECK(onstack_view.handled == 1);
    CHECK(onstack_view.handler_sp > onstack_view.call_sp);  // the handler ran above the thread's stack
    CHECK(onstack_view.depth_inside == 2);
    CHECK(onstack_view.depth_back == 1);
    CHECK(onstack_view.result == 5);
    CHECK(onstack_view.depth_after == 0);
}

const char *program = nullptr;  // argv[0], to start the test again in a child process

/// Runs this test program with `mode` in a child process, its stderr into `output` (or /dev/null);
/// returns its wait status.
int run_child_process(const char *mode, std::string *output) {
    char mode_argument[64];
    std::snprintf(mode_argument, sizeof mode_argument, "%s", mode);
    char *arguments[] = {const_cast<char *>(program), mode_argument, nullptr};
    char path[] = "/tmp/apkrt-host-call-XXXXXX";
    int file = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output != nullptr) {
        file = ::mkstemp(path);
        CHECK(file >= 0);
        posix_spawn_file_actions_adddup2(&actions, file, STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    pid_t child = 0;
    CHECK(::posix_spawn(&child, program, &actions, nullptr, arguments, nullptr) == 0);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    if (output != nullptr) {
        char buffer[4096];
        output->clear();
        for (off_t offset = 0;;) {
            const ssize_t count = ::pread(file, buffer, sizeof buffer, offset);
            if (count <= 0) break;
            output->append(buffer, static_cast<std::size_t>(count));
            offset += count;
        }
        ::close(file);
        ::unlink(path);
    }
    return status;
}

/// In a child process: the test's own thread as the program's first thread, as in main.
void become_program() {
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
}

int deep_level = 0;

Word deep_guest();

Word deep_host() {
    ++deep_level;
    std::fprintf(stderr, "depth %d\n", deep_level);
    if (deep_level > 20) return 0;
    return apkrt_call_guest(address_of(&deep_guest), 0, 0, 0, 0, 0, 0, 0, 0);
}

Word deep_guest() {
    return probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/// In the child process: host and guest code nest until the entry's stack of 16 is full.
int run_too_deep() {
    become_program();
    set_table(address_of(&deep_host));
    run_on_guest_thread([](void *) { deep_guest(); });
    return 3;  // the 17th call should have ended the process
}

// Decision 1: the stack of host calls holds 16, and a deeper call is a fault of the runtime.
TEST(host_calls_nested_deeper_than_16_are_a_runtime_fault) {
    std::string output;
    const int status = run_child_process("--too-deep", &output);
    CHECK(WIFSIGNALED(status));
    CHECK(output.find("depth 16\n") != std::string::npos);
    CHECK(output.find("depth 17\n") == std::string::npos);
    CHECK(output.find("apkrt: host calls nest deeper than 16") != std::string::npos);
}

volatile std::uintptr_t fault_address = 16;

Word host_fault() {
    *reinterpret_cast<volatile int *>(fault_address) = 1;
    return 0;
}

void on_segv(int, void *, void *) {
    static const char message[] = "the guest's SIGSEGV handler ran\n";
    static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
    ::_exit(42);
}

/// In the child process: a guest thread whose host call faults, with a guest handler for SIGSEGV.
int run_host_fault() {
    become_program();
    struct {
        Word handler, flags, restorer, mask;
    } action{address_of(&on_segv), sa_siginfo | sa_restorer, address_of(&probe_sigreturn), 0};
    if (probe_syscall(nr_rt_sigaction, sigsegv, address(&action), 0, 8) != 0) return 2;
    set_table(address_of(&host_fault));
    run_on_guest_thread([](void *) { probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0); });
    return 3;  // the fault should have ended the process
}

// Decision 1: a fault inside host code is the runtime's, even when guest code called it: the process
// dies as with any fault of the runtime, and the guest's SIGSEGV handler does not run.
TEST(a_fault_in_host_code_is_the_runtime_s_own) {
    const int status = run_child_process("--host-fault", nullptr);
    const bool guest_handler = WIFEXITED(status) && WEXITSTATUS(status) == 42;
    if (guest_handler) std::fprintf(stderr, "the fault in host code went to the guest's handler\n");
    CHECK(!guest_handler);
    CHECK(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 3));
}

// --- The handshake of A3 ---------------------------------------------------------------------------

struct HandshakeView {
    std::atomic<long> tid{0};
    std::atomic<int> ready{0};
    std::atomic<int> go{0};
    std::atomic<int> started{0};
    std::atomic<int> returning{0};
    int urgent_pending = -1;
    int slept = -1;
    pthread_t host{};
};
HandshakeView handshake_view;

/// Whether a runtime signal is pending on the calling thread; then the host's next syscall that an
/// unblocked one could interrupt would end with EINTR.
int runtime_signal_pending() {
    sigset_t pending;
    sigpending(&pending);
    return sigismember(&pending, apkrt::runtime_signal);
}

void block_runtime_signal() {
    sigset_t runtime;
    sigemptyset(&runtime);
    sigaddset(&runtime, apkrt::runtime_signal);
    ::pthread_sigmask(SIG_BLOCK, &runtime, nullptr);
}

Word host_check() {
    handshake_view.started = 1;
    handshake_view.urgent_pending = runtime_signal_pending();
    handshake_view.slept = nap(20'000'000);
    return 0;
}

void handshake_child(void *) {
    handshake_view.tid = own_tid();
    handshake_view.host = ::pthread_self();
    // Blocked, so that the runtime signal the test sends stays pending until something takes it.
    block_runtime_signal();
    handshake_view.ready = 1;
    wait_until(handshake_view.go);
    probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

/// The guest thread `tid`'s signal state; it must live while the test uses it.
apkrt::signals::ThreadSignals *state_of(long tid) {
    apkrt::signals::ThreadSignals *state = nullptr;
    apkrt::threads::find(static_cast<std::int32_t>(tid), [&](apkrt::threads::GuestThread &thread) { state = &thread.signals; });
    return state;
}

/// Plays another thread that looked at the target's in_runtime before it went into host code and sends
/// its runtime signal only now.
void finish_late_runtime_signal(apkrt::signals::ThreadSignals &state, pthread_t target) {
    ::pthread_kill(target, apkrt::runtime_signal);
    state.nudged.fetch_add(1);
    state.nudging.fetch_sub(1);
}

// A3: a host call's entry that finds another thread on its way with a runtime signal waits for it, and
// takes the signal before the host function's first instruction.
TEST(a_host_call_waits_for_a_runtime_signal_on_its_way_and_takes_it_first) {
    set_table(address_of(&host_check));
    handshake_view.ready = 0;
    handshake_view.go = 0;
    handshake_view.started = 0;
    handshake_view.urgent_pending = -1;
    handshake_view.slept = -1;
    Child child;
    child.start(handshake_child);
    wait_until(handshake_view.ready);
    auto *state = state_of(handshake_view.tid.load());
    CHECK(state != nullptr);
    if (state == nullptr) return;
    state->nudging.fetch_add(1);  // a sender between its look at in_runtime and its signal
    handshake_view.go = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(handshake_view.started.load() == 0);  // the entry waits for it
    finish_late_runtime_signal(*state, handshake_view.host);
    child.join();
    CHECK(handshake_view.started.load() == 1);
    CHECK(handshake_view.urgent_pending == 0);
    CHECK(handshake_view.slept == 0);
}

struct ArgumentsView {
    Word x[8] = {};
    double d[8] = {};
};
ArgumentsView arguments_view;

Word host_arguments(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7, double d0, double d1,
                    double d2, double d3, double d4, double d5, double d6, double d7) {
    arguments_view = {{a0, a1, a2, a3, a4, a5, a6, a7}, {d0, d1, d2, d3, d4, d5, d6, d7}};
    return 0;
}

void arguments_child(void *) {
    handshake_view.tid = own_tid();
    handshake_view.host = ::pthread_self();
    handshake_view.ready = 1;
    wait_until(handshake_view.go);
    probe_stub_3(0x1010, 0x2020, 0x3030, 0x4040, 0x5050, 0x6060, 0x7070, 0x8080, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5,
                 8.5);
}

// A3: while the entry waits for a runtime signal on its way, it keeps every argument register.
TEST(a_host_call_that_waits_for_a_runtime_signal_keeps_its_arguments) {
    set_table(0, 0, address_of(&host_arguments));
    handshake_view.ready = 0;
    handshake_view.go = 0;
    arguments_view = {};
    Child child;
    child.start(arguments_child);
    wait_until(handshake_view.ready);
    auto *state = state_of(handshake_view.tid.load());
    CHECK(state != nullptr);
    if (state == nullptr) return;
    state->nudging.fetch_add(1);  // a sender on its way: the entry takes its slow path
    handshake_view.go = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state->nudging.fetch_sub(1);  // it found host code and sent nothing
    child.join();
    for (int index = 0; index < 8; ++index) {
        CHECK(arguments_view.x[index] == static_cast<Word>(0x1010 * (index + 1)));
        CHECK(arguments_view.d[index] == 1.5 + index);
    }
}

// A3: a runtime signal that another thread sent before the host call, still pending when the thread
// makes it, arrives before the host function's first instruction.
TEST(a_runtime_signal_sent_before_a_host_call_arrives_before_host_code) {
    set_table(address_of(&host_check));
    handshake_view.ready = 0;
    handshake_view.go = 0;
    handshake_view.started = 0;
    handshake_view.urgent_pending = -1;
    handshake_view.slept = -1;
    Child child;
    child.start(handshake_child);
    wait_until(handshake_view.ready);
    auto *state = state_of(handshake_view.tid.load());
    CHECK(state != nullptr);
    if (state == nullptr) return;
    state->nudging.fetch_add(1);  // a sender that found guest code
    finish_late_runtime_signal(*state, handshake_view.host);
    handshake_view.go = 1;
    child.join();
    CHECK(handshake_view.started.load() == 1);
    CHECK(handshake_view.urgent_pending == 0);
    CHECK(handshake_view.slept == 0);
}

Word guest_returning() {
    block_runtime_signal();  // as in handshake_child: the test's late signal stays pending
    handshake_view.returning = 1;
    wait_until(handshake_view.go);  // the test's sender looks at in_runtime now, finding guest code
    return 0;
}

Word host_calling_back_then_checking() {
    apkrt_call_guest(address_of(&guest_returning), 0, 0, 0, 0, 0, 0, 0, 0);
    return host_check();
}

void callback_child(void *) {
    handshake_view.tid = own_tid();
    handshake_view.host = ::pthread_self();
    handshake_view.ready = 1;
    probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

// A2 and A3: apkrt_call_guest gives host code back only once no runtime signal is on its way.
TEST(a_callback_returns_to_host_code_only_once_no_runtime_signal_is_on_its_way) {
    set_table(address_of(&host_calling_back_then_checking));
    handshake_view.ready = 0;
    handshake_view.go = 0;
    handshake_view.started = 0;
    handshake_view.returning = 0;
    handshake_view.urgent_pending = -1;
    handshake_view.slept = -1;
    Child child;
    child.start(callback_child);
    wait_until(handshake_view.returning);
    auto *state = state_of(handshake_view.tid.load());
    CHECK(state != nullptr);
    if (state == nullptr) return;
    state->nudging.fetch_add(1);  // the callback runs guest code: a sender looks now
    handshake_view.go = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(handshake_view.started.load() == 0);  // apkrt_call_guest waits for it
    finish_late_runtime_signal(*state, handshake_view.host);
    child.join();
    CHECK(handshake_view.started.load() == 1);
    CHECK(handshake_view.urgent_pending == 0);
    CHECK(handshake_view.slept == 0);
}

struct StressView {
    std::atomic<long> tid{0};
    std::atomic<int> done{0};
    int interrupted = 0;
};
StressView stress_view;

Word host_nap() {
    if (nap(20'000) != 0) ++stress_view.interrupted;
    return 0;
}

std::uint64_t next_random(std::uint64_t &state) {  // xorshift64
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Decision 1 and A3 under load: another thread keeps sending guest signals while the thread goes in and
// out of host calls, and no host sleep ever ends with EINTR.
TEST(no_runtime_signal_reaches_host_code_while_signals_keep_coming) {
    set_table(address_of(&host_nap));
    stress_view.tid = 0;
    stress_view.done = 0;
    stress_view.interrupted = 0;
    const int before = seen.count.load();
    Child child;
    child.start([](void *) {
        stress_view.tid = own_tid();
        for (int round = 0; round < 2'000; ++round) probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        stress_view.done = 1;
    });
    while (stress_view.tid.load() == 0) std::this_thread::yield();
    std::uint64_t random = 0x2545'f491'4f6c'dd1d;
    while (stress_view.done.load() == 0) {
        const long sent = send(stress_view.tid.load(), sigusr1);
        CHECK(sent == 0 || (sent == -3 && stress_view.done.load() != 0));  // ESRCH once the child ended
        const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(next_random(random) % 20'000);
        while (std::chrono::steady_clock::now() < until) {
        }
    }
    child.join();
    CHECK(seen.count.load() - before >= 100);  // enough signals came
    if (stress_view.interrupted != 0) std::fprintf(stderr, "%d host sleeps ended with EINTR\n", stress_view.interrupted);
    CHECK(stress_view.interrupted == 0);
}

// --- Binding a front lib's table (decision 3) -------------------------------------------------------

struct ProviderView {
    int calls = 0;
    void *context = nullptr;
    void **table = nullptr;
    std::size_t length = 0;
    Word hash = 0;
    int side = -1;
    int guest = -1;
    int answer = 0;
    bool nap = false;
    int slept = -1;
    std::atomic<int> inside{0};

    void reset(int refusal, bool with_nap) {
        calls = 0;
        context = nullptr;
        table = nullptr;
        length = 0;
        hash = 0;
        side = -1;
        guest = -1;
        answer = refusal;
        nap = with_nap;
        slept = -1;
        inside = 0;
    }
};
ProviderView provider_view;
int provider_context;

Word host_bound(Word a0) {
    return a0 + 1000;
}

int provider(void *context, void **table, std::size_t length, Word hash) {
    auto &view = provider_view;
    ++view.calls;
    view.context = context;
    view.table = table;
    view.length = length;
    view.hash = hash;
    view.side = side();
    view.guest = apkrt_is_guest_thread();
    view.inside = 1;
    if (view.nap) view.slept = nap(100'000'000);
    if (view.answer != 0) return view.answer;
    for (std::size_t slot = 1; slot < length; ++slot) table[slot] = reinterpret_cast<void *>(&host_bound);
    return 0;
}

long bind(const char *name, Word *table, Word length, Word hash) {
    return probe_syscall(APKRT_NR_BIND_HOST_TABLE, address(name), address(table), length, hash);
}

struct BindView {
    std::atomic<long> tid{0};
    std::atomic<int> done{0};
    long result = 1;
    Word called = 0;
};
BindView bind_view;

// Decision 3: the syscall hands the table to the provider set for the lib's name, which runs on the
// calling guest thread as host code, and puts the entry into slot 0; a stub then reaches the host
// function. A guest signal sent while the provider works waits for the syscall's end.
TEST(a_front_lib_binds_its_table_through_graphics_host_s_provider) {
    CHECK(apkrt_set_host_provider("test-lib", provider, &provider_context) == 0);
    probe_table[0] = probe_table[1] = probe_table[2] = probe_table[3] = 0;
    provider_view.reset(0, true);
    bind_view.tid = 0;
    bind_view.done = 0;
    const int before = seen.count.load();
    Child child;
    child.start([](void *) {
        bind_view.tid = own_tid();
        bind_view.result = bind("test-lib", probe_table, 4, 0x1234'5678'9abc'def0ull);
        if (bind_view.result == 0) bind_view.called = probe_stub_1(7, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        bind_view.done = 1;
    });
    while (provider_view.inside.load() == 0 && bind_view.done.load() == 0) std::this_thread::yield();
    if (provider_view.inside.load() != 0) CHECK(send(bind_view.tid.load(), sigusr1) == 0);
    child.join();
    CHECK(bind_view.result == 0);
    CHECK(provider_view.calls == 1);
    CHECK(provider_view.context == &provider_context);
    CHECK(provider_view.table == reinterpret_cast<void **>(probe_table));
    CHECK(provider_view.length == 4);
    CHECK(provider_view.hash == 0x1234'5678'9abc'def0ull);
    CHECK(provider_view.side == apkrt::signals::host_code);
    CHECK(provider_view.guest == 1);
    CHECK(provider_view.slept == 0);
    CHECK(seen.count.load() - before == 1);
    CHECK(probe_table[0] == address_of(&apkrt_host_call));
    CHECK(probe_table[3] == address_of(&host_bound));
    CHECK(bind_view.called == 1007);
    CHECK(apkrt_set_host_provider("test-lib", nullptr, nullptr) == 0);
}

Word unbound_table[3];
long bind_results[6];

// Decision 3: without a provider, when the provider refuses, and with a length of 0, a name of 64 bytes
// or a NULL name or table the syscall fails, and slot 0 stays empty, so the front lib answers "no
// driver".
TEST(binding_fails_without_a_provider_and_when_the_provider_refuses) {
    provider_view.reset(1, false);
    CHECK(apkrt_set_host_provider("refusing-lib", provider, nullptr) == 0);
    run_on_guest_thread([](void *) {
        static const std::string long_name(64, 'x');
        bind_results[0] = bind("no-such-lib", unbound_table, 3, 0);
        bind_results[1] = bind("refusing-lib", unbound_table, 3, 0);
        bind_results[2] = bind("refusing-lib", unbound_table, 0, 0);
        bind_results[3] = bind(long_name.c_str(), unbound_table, 3, 0);
        bind_results[4] = bind(nullptr, unbound_table, 3, 0);
        bind_results[5] = bind("refusing-lib", nullptr, 3, 0);
    });
    CHECK(bind_results[0] == -enoent);
    CHECK(bind_results[1] == -einval);
    CHECK(bind_results[2] == -einval);
    CHECK(bind_results[3] == -einval);
    CHECK(bind_results[4] == -einval);
    CHECK(bind_results[5] == -einval);
    CHECK(provider_view.calls == 1);  // only the second bind reaches it
    CHECK(unbound_table[0] == 0);
    CHECK(apkrt_set_host_provider("refusing-lib", nullptr, nullptr) == 0);
    run_on_guest_thread([](void *) { bind_results[0] = bind("refusing-lib", unbound_table, 3, 0); });
    CHECK(bind_results[0] == -enoent);
    CHECK(apkrt_set_host_provider("", provider, nullptr) == -1);
    CHECK(apkrt_set_host_provider(std::string(64, 'y').c_str(), provider, nullptr) == -1);
}

// --- Cost ------------------------------------------------------------------------------------------

Word host_increment(Word a0) {
    return a0 + 1;
}

Word (*volatile direct_function)(Word) = host_increment;

// Decision 1: a few instructions per call. Measured, not checked: ns per call through a plain stub and
// the entry on a guest thread, against a direct call.
TEST(a_host_call_costs_a_few_instructions) {
    set_table(address_of(&host_increment));
    run_on_guest_thread([](void *) {
        constexpr int rounds = 1'000'000;
        Word value = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int round = 0; round < rounds; ++round) value = probe_stub_1(value, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        const auto middle = std::chrono::steady_clock::now();
        for (int round = 0; round < rounds; ++round) value = direct_function(value);
        const auto end = std::chrono::steady_clock::now();
        CHECK(value == 2 * rounds);
        const auto per_call = [](auto span) { return std::chrono::duration<double, std::nano>(span).count() / rounds; };
        std::fprintf(stderr, "host call through the entry: %.2f ns, direct call: %.2f ns\n", per_call(middle - start),
                     per_call(end - middle));
    });
}

}  // namespace

int main(int argc, char **argv) {
    program = argv[0];
    if (argc > 1 && std::strcmp(argv[1], "--too-deep") == 0) return run_too_deep();
    if (argc > 1 && std::strcmp(argv[1], "--host-fault") == 0) return run_host_fault();
    // The test's own thread is the program's first thread: the process lives as long as it does.
    become_program();
    install_handler();
    return apk::testing::run_all();
}
