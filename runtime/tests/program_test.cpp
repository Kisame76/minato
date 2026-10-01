// Test 1 of ADR-0028, step a): the private syscall numbers (decision 11), the program's start on a
// thread of its own (apkrt_start_program) and its end with the session's end handler and the stopped
// threads (decisions 3 and 5). Whatever starts or ends a program runs in a child process, this test
// started again with the case's name, since a process runs one program. There the guest threads are
// clone children that make their syscalls through the veneer's exact instructions (threads_probe.S,
// blocking_probe.S) and call host code the way a front lib's stubs do (host_call_probe.S); the programs
// are a static one the converter makes and the NDK fixture raw-dyn without its library (skipped while
// it is missing). That the start-up code runs on the program's thread, fixtures/suite/start-test.c
// checks against Android.
#include "loader_syscalls.h"
#include "syscall_frame.h"
#include "threads.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "check.h"
#include "elf_builder.h"

#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
long probe_syscall6(Word number, Word a0, Word a1, Word a2, Word a3, Word a4, Word a5);

// host_call_probe.S
Word probe_table[4];
struct ProbeResults {
    Word x[2];
    __uint128_t q[4];
};
ProbeResults probe_results;
Word probe_stub_1(Word a0, Word a1, Word a2, Word a3, Word a4, Word a5, Word a6, Word a7, Word s0, Word s1);
}

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, eventpoll.h, futex.h.
constexpr Word pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr Word nr_epoll_create1 = 20, nr_epoll_ctl = 21, nr_epoll_pwait = 22, nr_close = 57, nr_pipe2 = 59,
               nr_read = 63, nr_write = 64, nr_exit_group = 94, nr_futex = 98, nr_rt_sigprocmask = 135;
constexpr Word epoll_ctl_add = 1, epollin = 1, futex_wait = 0, futex_wake = 1, sig_block = 0;

/// struct epoll_event on arm64: not packed, 16 bytes.
struct EpollEvent {
    std::uint32_t events;
    Word data;
};

Word address(const void *pointer) {
    return reinterpret_cast<Word>(pointer);
}

template <typename Function>
Word address_of(Function *function) {
    return reinterpret_cast<Word>(function);
}

template <typename Condition>
bool wait_for(Condition condition, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// --- The child process ---------------------------------------------------------------------------------

/// A clone child, a guest thread of its own, on a stack the test maps and never unmaps: the process
/// ends with the case.
struct Child {
    int tid_word = 0;
    int tid = 0;

    void start(void (*function)(void *), void *argument = nullptr) {
        constexpr std::size_t stack_size = 256 * 1024;
        void *stack = ::mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(stack != MAP_FAILED);
        const long result = probe_clone(pthread_flags, static_cast<char *>(stack) + stack_size, &tid_word, nullptr,
                                        &tid_word, function, argument);
        CHECK(result > 0);
        tid = static_cast<int>(result);
    }

    /// Whether the thread has stopped for good (ADR-0028, decision 5).
    bool stopped() const {
        bool stopped = false;
        apkrt::threads::find(tid, [&](apkrt::threads::GuestThread &thread) { stopped = thread.stopped.load(); });
        return stopped;
    }

    /// Whether the thread sleeps in the kernel inside a syscall, as a blocking one leaves it, and still a
    /// moment later: a thread that only waits for a lock on its way goes on at once.
    bool sleeps_in_syscall() const {
        const auto asleep = [&] {
            bool asleep = false;
            apkrt::threads::find(tid, [&](apkrt::threads::GuestThread &thread) {
                const pthread_t host = thread.signals.host.load();
                if (host == pthread_t{} || thread.signals.in_runtime.load() != apkrt::signals::runtime_code) return;
                thread_basic_info_data_t info;
                mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
                asleep = ::thread_info(::pthread_mach_thread_np(host), THREAD_BASIC_INFO,
                                       reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS &&
                         info.run_state == TH_STATE_WAITING;
            });
            return asleep;
        };
        if (!asleep()) return false;
        std::this_thread::sleep_for(2ms);
        return asleep();
    }
};

/// What the end handler saw.
struct EndView {
    std::atomic<int> calls{0};
    std::atomic<int> status{-1};
    std::atomic<bool> called{false};
    std::string report;  // what the report descriptor held when the handler ran
    bool runtime_signal_blocked = false;  // so that none ends the handler's host calls with EINTR
    int report_read_end = -1;
};
EndView end_view;

void on_end(void *context, int status) {
    auto &view = *static_cast<EndView *>(context);
    if (view.calls.fetch_add(1) == 0) {
        view.status.store(status);
        char buffer[16];
        const ssize_t count = ::read(view.report_read_end, buffer, sizeof buffer);
        if (count > 0) view.report.assign(buffer, static_cast<std::size_t>(count));
        sigset_t blocked;
        ::pthread_sigmask(SIG_BLOCK, nullptr, &blocked);
        view.runtime_signal_blocked = sigismember(&blocked, SIGURG) == 1;
    }
    view.called.store(true);
}

/// A session whose end handler notes the call, and the exit status reported into a pipe it reads.
void begin_session() {
    int ends[2];
    CHECK(::pipe(ends) == 0);
    CHECK(::fcntl(ends[0], F_SETFL, O_NONBLOCK) == 0);
    end_view.report_read_end = ends[0];
    apkrt_report_exit_status(ends[1]);
    const apkrt_session session{.context = &end_view, .end = on_end};
    CHECK(apkrt_session_begin(&session) == 0);
}

/// Waits for the end handler, which must have been called once with `status` after the report.
void check_end(int status) {
    CHECK(wait_for([] { return end_view.called.load(); }));
    CHECK(end_view.status.load() == status);
    CHECK(end_view.report == std::to_string(status) + "\n");
    CHECK(end_view.runtime_signal_blocked);
    CHECK(end_view.calls.load() == 1);
}

/// How a case that passed ends its process: with no exit status its program uses.
constexpr int case_passed = 40;

[[noreturn]] void finish_case() {
    std::fflush(stderr);
    ::_exit(apk::testing::failures == 0 ? case_passed : 1);
}

/// The start: the program in `container` waits for a byte on its standard input, and apkrt_start_program
/// must return meanwhile; then the byte is its exit status.
[[noreturn]] void run_start_returns(const char *container) {
    int input[2];
    CHECK(::pipe(input) == 0);
    CHECK(::dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
    ::close(input[0]);
    begin_session();
    static std::atomic<bool> returned{false};
    std::thread([] {
        std::this_thread::sleep_for(10s);
        if (returned.load()) return;
        static const char message[] = "apkrt_start_program did not return while the program ran\n";
        static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
        ::_exit(3);
    }).detach();
    const char *const argv[] = {"./wait-program", nullptr};
    const char *const envp[] = {nullptr};
    char error[512] = "";
    const int result = apkrt_start_program(container, argv, envp, error, sizeof error);
    returned.store(true);
    if (result != 0) std::fprintf(stderr, "apkrt_start_program: %s\n", error);
    CHECK(result == 0);
    // One program per process: a second start is refused, and the first program goes on.
    char second[512] = "";
    CHECK(apkrt_start_program(container, argv, envp, second, sizeof second) == -1);
    CHECK(std::string_view(second).find("one program per process") != std::string_view::npos);
    CHECK(end_view.calls.load() == 0);
    CHECK(::write(input[1], "\x06", 1) == 1);
    check_end(6);
    finish_case();
}

struct Race {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
};
Race race;

/// Guest code: exit_group with the status in `argument` once every racer is ready.
void exit_group_at_go(void *argument) {
    race.ready.fetch_add(1);
    while (!race.go.load()) {
    }
    probe_syscall(nr_exit_group, reinterpret_cast<Word>(argument));
}

/// Two threads call exit_group at once: one end, one handler call, and both threads stop.
[[noreturn]] void run_exit_group() {
    begin_session();
    Child first, second;
    first.start(exit_group_at_go, reinterpret_cast<void *>(7));
    second.start(exit_group_at_go, reinterpret_cast<void *>(8));
    CHECK(wait_for([] { return race.ready.load() == 2; }));
    race.go.store(true);
    CHECK(wait_for([] { return end_view.called.load(); }));
    const int status = end_view.status.load();
    CHECK(status == 7 || status == 8);
    check_end(status);
    // The thread whose end it was stops once its handler returned, the other one at once.
    CHECK(wait_for([&] { return first.stopped() && second.stopped(); }));
    // An end after the first only stops its thread, whenever it comes: here a host thread's exit_group.
    std::thread([] { probe_syscall(nr_exit_group, 9); }).detach();
    std::this_thread::sleep_for(100ms);
    CHECK(end_view.calls.load() == 1);
    finish_case();
}

/// Guest code: a present at the window `argument`, whose gate is closed (ADR-0028, decision 7).
void present_at_the_gate(void *argument) {
    probe_syscall(APKRT_NR_WINDOW_GATE, reinterpret_cast<Word>(argument), APKRT_GATE_PRESENT);
}

/// The session at the program's end (ADR-0028, decisions 2 and 7): a host thread's wait for an answer that
/// will not come ends with -1, every post from then on fails, and a guest thread that waits at a gate
/// stops.
[[noreturn]] void run_session_ends() {
    begin_session();
    const auto serial = apkrt_session_post(APKRT_EVENT_CREATE, 0, 0, 0);
    CHECK(serial == 1);
    static std::atomic<int> waited{2};
    std::thread host([serial] { waited.store(apkrt_session_wait(serial, 30.0)); });
    std::this_thread::sleep_for(50ms);
    CHECK(waited.load() == 2);
    Child presenting;
    presenting.start(present_at_the_gate, reinterpret_cast<void *>(apkrt_window_add(nullptr, 1080, 2340)));
    CHECK(wait_for([&] { return presenting.sleeps_in_syscall(); }));
    Child child;
    child.start(exit_group_at_go, reinterpret_cast<void *>(5));
    CHECK(wait_for([] { return race.ready.load() == 1; }));
    race.go.store(true);
    check_end(5);
    CHECK(wait_for([] { return waited.load() == -1; }, 2s));
    CHECK(apkrt_session_post(APKRT_EVENT_DESTROY, 0, 0, 0) == 0);
    CHECK(wait_for([&] { return presenting.stopped(); }, 2s));
    host.join();
    finish_case();
}

/// The channel when activity-host closes its end (ADR-0028, decision 2): posts fail from then on, and the
/// host gets no SIGPIPE, which this case does not ignore.
[[noreturn]] void run_reader_goes() {
    ::signal(SIGPIPE, SIG_DFL);
    begin_session();
    apkrt_configuration configuration{};
    const long events = probe_syscall(APKRT_NR_SESSION_CONNECT, address(&configuration));
    CHECK(events >= 0);
    CHECK(apkrt_session_post(APKRT_EVENT_CREATE, 0, 0, 0) == 1);
    CHECK(probe_syscall(nr_close, static_cast<Word>(events)) == 0);
    CHECK(apkrt_session_post(APKRT_EVENT_START, 0, 0, 0) == 0);
    CHECK(apkrt_session_post(APKRT_EVENT_RESUME, 0, 0, 0) == 0);
    CHECK(apkrt_session_wait(1, 5.0) == -1);
    finish_case();
}

/// Guest code: returns at go, and the probe ends the thread with exit(0).
void return_at_go(void *) {
    race.ready.fetch_add(1);
    while (!race.go.load()) {
    }
}

/// The program's last thread ends with exit: its end is the program's, with the first thread's exit
/// code, which no thread here has: 0.
[[noreturn]] void run_last_thread() {
    begin_session();
    Child first, second;
    first.start(return_at_go);
    second.start(return_at_go);
    CHECK(wait_for([] { return race.ready.load() == 2; }));
    race.go.store(true);
    check_end(0);
    finish_case();
}

struct StopView {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<long> spins{0};
    std::atomic<int> read_returned{0};
    std::atomic<int> poll_returned{0};
    std::atomic<int> futex_returned{0};
    int read_pipe[2] = {-1, -1};  // the guest's, for the thread that reads
    int poll_pipe[2] = {-1, -1};  // the guest's, which the epoll watches
    int epoll = -1;
    std::uint32_t futex_word = 0;
};
StopView stop_view;

void spin(void *) {
    stop_view.ready.fetch_add(1);
    for (;;) stop_view.spins.fetch_add(1, std::memory_order_relaxed);
}

void read_pipe(void *) {
    stop_view.ready.fetch_add(1);
    char byte = 0;
    probe_syscall(nr_read, static_cast<Word>(stop_view.read_pipe[0]), address(&byte), 1);
    stop_view.read_returned.store(1);
}

void poll_epoll(void *) {
    stop_view.ready.fetch_add(1);
    EpollEvent events[1] = {};
    probe_syscall6(nr_epoll_pwait, static_cast<Word>(stop_view.epoll), address(events), 1, static_cast<Word>(-1), 0, 8);
    stop_view.poll_returned.store(1);
}

/// It blocks every signal first: the end's mark is SIGKILL, which no mask blocks.
void sleep_in_futex(void *) {
    const Word every_signal = ~Word{0};
    probe_syscall(nr_rt_sigprocmask, sig_block, address(&every_signal), 0, 8);
    stop_view.ready.fetch_add(1);
    probe_syscall(nr_futex, address(&stop_view.futex_word), futex_wait, 0, 0);
    stop_view.futex_returned.store(1);
}

void exit_group_when_told(void *) {
    while (!stop_view.go.load()) {
    }
    probe_syscall(nr_exit_group, 5);
}

/// exit_group stops every other guest thread, wherever it is: in guest code, in a pipe read, in
/// epoll_pwait, in a futex wait. None of them runs guest code again, whatever wakes it afterwards.
[[noreturn]] void run_stop() {
    begin_session();
    CHECK(probe_syscall(nr_pipe2, address(stop_view.read_pipe), 0) == 0);
    CHECK(probe_syscall(nr_pipe2, address(stop_view.poll_pipe), 0) == 0);
    stop_view.epoll = static_cast<int>(probe_syscall(nr_epoll_create1, 0));
    CHECK(stop_view.epoll >= 0);
    EpollEvent watched{static_cast<std::uint32_t>(epollin), 1};
    CHECK(probe_syscall(nr_epoll_ctl, static_cast<Word>(stop_view.epoll), epoll_ctl_add,
                        static_cast<Word>(stop_view.poll_pipe[0]), address(&watched)) == 0);
    Child spinner, reader, poller, sleeper, ender;
    spinner.start(spin);
    reader.start(read_pipe);
    poller.start(poll_epoll);
    sleeper.start(sleep_in_futex);
    ender.start(exit_group_when_told);
    CHECK(wait_for([] { return stop_view.ready.load() == 4; }));
    CHECK(wait_for([&] { return reader.sleeps_in_syscall() && poller.sleeps_in_syscall() && sleeper.sleeps_in_syscall(); }));
    stop_view.go.store(true);
    check_end(5);
    CHECK(wait_for([&] {
        return spinner.stopped() && reader.stopped() && poller.stopped() && sleeper.stopped() && ender.stopped();
    }));
    const long spins = stop_view.spins.load();
    std::this_thread::sleep_for(20ms);
    CHECK(stop_view.spins.load() == spins);
    const char byte = 'x';
    CHECK(probe_syscall(nr_write, static_cast<Word>(stop_view.read_pipe[1]), address(&byte), 1) == 1);
    CHECK(probe_syscall(nr_write, static_cast<Word>(stop_view.poll_pipe[1]), address(&byte), 1) == 1);
    stop_view.futex_word = 1;
    probe_syscall(nr_futex, address(&stop_view.futex_word), futex_wake, INT_MAX);
    std::this_thread::sleep_for(20ms);
    CHECK(stop_view.read_returned.load() == 0);
    CHECK(stop_view.poll_returned.load() == 0);
    CHECK(stop_view.futex_returned.load() == 0);
    // A thread that starts after the end stops before its first instruction of guest code.
    static std::atomic<int> late_ran{0};
    Child late;
    late.start([](void *) { late_ran.store(1); });
    CHECK(wait_for([&] { return late.stopped(); }));
    CHECK(late_ran.load() == 0);
    finish_case();
}

struct HostCallView {
    std::atomic<int> inside{0};
    std::atomic<int> interrupted{0};
    std::atomic<int> returned{0};
    int host_pipe[2] = {-1, -1};  // the host's
};
HostCallView host_call_view;

/// Host code: a read the runtime signal would interrupt with EINTR, since its handler has no SA_RESTART.
Word read_in_host_code(Word, Word, Word, Word, Word, Word, Word, Word, Word, Word) {
    host_call_view.inside.store(1);
    char byte = 0;
    while (::read(host_call_view.host_pipe[0], &byte, 1) < 0) {
        if (errno != EINTR) break;
        host_call_view.interrupted.fetch_add(1);
    }
    return 0;
}

void call_host(void *) {
    probe_stub_1(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    host_call_view.returned.store(1);
}

void exit_group_once_inside(void *) {
    while (host_call_view.inside.load() == 0) {
    }
    probe_syscall(nr_exit_group, 3);
}

/// A thread inside a host call gets no runtime signal at the end (ADR-0030, decision 1): it goes on in
/// host code and stops when the call returns, without going back to guest code.
[[noreturn]] void run_host_call() {
    begin_session();
    CHECK(::pipe(host_call_view.host_pipe) == 0);
    probe_table[0] = address_of(&apkrt_host_call);
    probe_table[1] = address_of(&read_in_host_code);
    Child caller, ender;
    caller.start(call_host);
    ender.start(exit_group_once_inside);
    check_end(3);
    CHECK(wait_for([&] { return ender.stopped(); }));
    std::this_thread::sleep_for(50ms);  // time for a runtime signal that should not come
    CHECK(host_call_view.interrupted.load() == 0);
    CHECK(!caller.stopped());
    CHECK(::write(host_call_view.host_pipe[1], "x", 1) == 1);
    CHECK(wait_for([&] { return caller.stopped(); }));
    std::this_thread::sleep_for(20ms);
    CHECK(host_call_view.returned.load() == 0);
    finish_case();
}

/// XNU drops the runtime signal that resume() sends its own thread once another thread's exit is under way, and
/// the thread runs on for a moment (graphics.gles_glue: the end handler's _exit met the app's thread on its way
/// back into guest code). SIG_IGN drops it the same way, for as long as the case wants. Before the program's end
/// the thread sends the signal again until it arrives; after the end it stops, as the signal's handler would have
/// stopped it, without a report.
[[noreturn]] void run_dropped_resume() {
    begin_session();
    CHECK(probe_syscall(nr_pipe2, address(stop_view.read_pipe), 0) == 0);
    Child reader, ender;
    reader.start(read_pipe);  // the runtime's handlers are in place from its start on
    ender.start(exit_group_when_told);
    CHECK(wait_for([&] { return reader.sleeps_in_syscall(); }));
    struct sigaction runtime {};
    CHECK(::sigaction(SIGURG, nullptr, &runtime) == 0);
    ::signal(SIGURG, SIG_IGN);
    static std::atomic<int> early_ran{0};
    Child early;
    early.start([](void *) { early_ran.store(1); });
    std::this_thread::sleep_for(20ms);
    CHECK(early_ran.load() == 0);
    CHECK(::sigaction(SIGURG, &runtime, nullptr) == 0);
    CHECK(wait_for([] { return early_ran.load() == 1; }));
    // The end wakes the reader, which goes back to guest code for its SIGKILL.
    ::signal(SIGURG, SIG_IGN);
    stop_view.go.store(true);
    check_end(5);
    CHECK(wait_for([&] { return reader.stopped() && ender.stopped(); }));
    CHECK(stop_view.read_returned.load() == 0);
    static std::atomic<int> late_ran{0};
    Child late;
    late.start([](void *) { late_ran.store(1); });
    CHECK(wait_for([&] { return late.stopped(); }));
    CHECK(late_ran.load() == 0);
    finish_case();
}

/// A runtime signal that never arrives: the thread gives up after a second and ends the process, as before.
[[noreturn]] void run_resume_never() {
    static std::atomic<int> first_ran{0};
    Child first;  // the runtime's handlers are in place from its start on; it lives on, so its end is not the end
    first.start([](void *) {
        first_ran.store(1);
        while (first_ran.load() != 2) {
        }
    });
    CHECK(wait_for([] { return first_ran.load() == 1; }));
    ::signal(SIGURG, SIG_IGN);
    Child never;
    never.start([](void *) {});
    std::this_thread::sleep_for(10s);
    ::_exit(3);  // the thread should have ended the process
}

/// linker64's failure ends the program as well: the dynamic program in `container` lacks its library.
[[noreturn]] void run_cannot_link(const char *container) {
    begin_session();
    const char *const argv[] = {"./raw-dyn", nullptr};
    const char *const envp[] = {nullptr};
    char error[512] = "";
    CHECK(apkrt_start_program(container, argv, envp, error, sizeof error) == 0);
    check_end(1);
    finish_case();
}

/// Without a session the program's end ends the process, as before: exit_group(9), reported on
/// descriptor 3.
[[noreturn]] void run_no_session() {
    apkrt_report_exit_status(3);
    Child ender;
    ender.start([](void *) { probe_syscall(nr_exit_group, 9); });
    std::this_thread::sleep_for(10s);
    ::_exit(2);  // exit_group should have ended the process
}

[[noreturn]] void run_case(std::string_view mode, const char *argument) {
    if (mode == "--start-returns") run_start_returns(argument);
    if (mode == "--exit-group") run_exit_group();
    if (mode == "--last-thread") run_last_thread();
    if (mode == "--session-ends") run_session_ends();
    if (mode == "--reader-goes") run_reader_goes();
    if (mode == "--stop") run_stop();
    if (mode == "--host-call") run_host_call();
    if (mode == "--dropped-resume") run_dropped_resume();
    if (mode == "--resume-never") run_resume_never();
    if (mode == "--cannot-link") run_cannot_link(argument);
    if (mode == "--no-session") run_no_session();
    std::fprintf(stderr, "unknown case %.*s\n", static_cast<int>(mode.size()), mode.data());
    ::_exit(2);
}

// --- The test ----------------------------------------------------------------------------------------

const char *program = nullptr;  // argv[0], to start the test again in a child process
std::filesystem::path scratch;
std::filesystem::path fixtures;  // tools/build-fixtures.sh's

struct Ended {
    int status = 0;      // as waitpid returns it
    std::string output;  // the child's stderr
    std::string report;  // what the runtime reported on the child's descriptor 3
};

/// Runs this test with `mode` in a child process and waits for its end, which shows its stderr unless
/// it ended with `expected`.
Ended run_child(const char *mode, const std::string &argument = "", int expected = case_passed) {
    Ended ended;
    int report[2];
    CHECK(::pipe(report) == 0);
    const auto output_path = scratch / "output.txt";
    const int output = ::open(output_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(output >= 0);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
    posix_spawn_file_actions_adddup2(&actions, report[1], 3);
    posix_spawn_file_actions_addclose(&actions, report[0]);
    std::string mode_text = mode;
    std::string argument_text = argument;
    char *arguments[] = {const_cast<char *>(program), mode_text.data(), argument_text.data(), nullptr};
    pid_t child = 0;
    CHECK(::posix_spawn(&child, program, &actions, nullptr, arguments, nullptr) == 0);
    posix_spawn_file_actions_destroy(&actions);
    ::close(report[1]);
    char buffer[256];
    for (ssize_t count; (count = ::read(report[0], buffer, sizeof buffer)) > 0;) {
        ended.report.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(report[0]);
    CHECK(::waitpid(child, &ended.status, 0) == child);
    for (off_t offset = 0;;) {
        const ssize_t count = ::pread(output, buffer, sizeof buffer, offset);
        if (count <= 0) break;
        ended.output.append(buffer, static_cast<std::size_t>(count));
        offset += count;
    }
    ::close(output);
    if (!(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == expected)) {
        std::fprintf(stderr, "%s: wait status %#x, stderr:\n%s", mode, ended.status, ended.output.c_str());
    }
    return ended;
}

bool succeeded(const Ended &ended) {
    return WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == case_passed;
}

/// The ELF file `elf` as <name>.dylib, a container signed ad hoc, in the scratch directory `directory`.
std::string write_container(const std::string &name, const std::vector<std::byte> &elf,
                            const std::filesystem::path &directory) {
    auto container = apk::container::convert(elf, {.install_name = "@rpath/" + name + ".dylib"}).bytes;
    apk::codesign::sign(container, {.identifier = name});
    std::filesystem::create_directories(directory);
    const auto path = directory / (name + ".dylib");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(container.data()), static_cast<std::streamsize>(container.size()));
    return path.string();
}

/// A static program that reads a byte from its standard input and ends with it as its exit status.
std::string wait_program() {
    namespace elf = apk::testing;
    constexpr std::uint32_t code[] = {
        0xd100'43ff,  // 0x200 sub  sp, sp, #16
        0xd280'0000,  // 0x204 mov  x0, #0            standard input
        0x9100'03e1,  // 0x208 mov  x1, sp
        0xd280'0022,  // 0x20c mov  x2, #1
        0xd280'07e8,  // 0x210 mov  x8, #63           read
        0xd400'0001,  // 0x214 svc  #0
        0x3940'03e0,  // 0x218 ldrb w0, [sp]
        0xd280'0bc8,  // 0x21c mov  x8, #94           exit_group
        0xd400'0001,  // 0x220 svc  #0
        0xd420'0020,  // 0x224 brk  #1
    };
    elf::ElfBuilder builder;
    builder.put<std::uint64_t>(24, 0x200);  // e_entry
    builder.segments({{elf::pt_load, elf::r | elf::x, 0, 0, 0x228, 0x228, 0x4000}});
    std::memcpy(builder.bytes.data() + 0x200, code, sizeof code);
    builder.sections({{".text", elf::sht_progbits, elf::shf_alloc | elf::shf_execinstr, 0x200, 0x200, sizeof code}});
    return write_container("wait-program", builder.bytes, scratch / "wait-program");
}

// Decision 11: the runtime's own syscalls follow the loader's 4096 to 4100, in the order of the ADR.
TEST(the_private_syscalls_follow_the_loader_s) {
    CHECK(APKRT_NR_DLOPEN == 4096 && APKRT_NR_DLADDR == 4100);
    CHECK(APKRT_NR_BIND_HOST_TABLE == 4101);
    CHECK(APKRT_NR_SESSION_CONNECT == 4102);
    CHECK(APKRT_NR_SESSION_ANSWER == 4103);
    CHECK(APKRT_NR_SESSION_REQUEST == 4104);
    CHECK(APKRT_NR_WINDOW_GATE == 4105);
}

// Decision 3: a session has an end handler, and a process has one session.
TEST(a_session_needs_an_end_handler_and_begins_once) {
    CHECK(apkrt_session_begin(nullptr) == -1);
    const apkrt_session without{.context = nullptr, .end = nullptr};
    CHECK(apkrt_session_begin(&without) == -1);
    const apkrt_session session{.context = nullptr, .end = [](void *, int) {}};
    CHECK(apkrt_session_begin(&session) == 0);
    CHECK(apkrt_session_begin(&session) == -1);
}

// Decision 5: what keeps a program from starting comes back to the caller, which keeps its thread.
TEST(a_program_that_cannot_start_gives_the_reason) {
    const char *const argv[] = {"./missing", nullptr};
    const char *const envp[] = {nullptr};
    char error[512] = "";
    CHECK(apkrt_start_program("/nonexistent/missing.dylib", argv, envp, error, sizeof error) == -1);
    CHECK(std::string_view(error).find("/nonexistent/missing.dylib") != std::string_view::npos);
}

// Decision 5: the start returns once the program runs, on a thread of its own.
TEST(the_start_returns_while_the_program_runs) {
    CHECK(succeeded(run_child("--start-returns", wait_program())));
}

// Decision 5: exit_group reports the status, then calls the end handler once on its thread, also when
// two threads end the program at once.
TEST(exit_group_calls_the_end_handler_once_after_the_report) {
    CHECK(succeeded(run_child("--exit-group")));
}

// Decision 5: the last thread's end is the program's end as well.
TEST(the_last_thread_s_end_calls_the_end_handler) {
    CHECK(succeeded(run_child("--last-thread")));
}

// Decisions 2 and 7: the program's end ends the host's waits for answers, posts fail from then on, and a
// thread at a window's gate stops.
TEST(the_program_s_end_ends_the_waits_for_answers_and_the_posts) {
    CHECK(succeeded(run_child("--session-ends")));
}

// Decision 2: a reader that goes away breaks the channel for good, and the host gets no SIGPIPE.
TEST(a_reader_that_goes_away_breaks_the_channel_without_sigpipe) {
    CHECK(succeeded(run_child("--reader-goes")));
}

// Decision 5: the other guest threads stop, whether they spin in guest code or wait in a pipe read, in
// epoll_pwait or in a futex.
TEST(the_program_s_end_stops_every_other_guest_thread) {
    CHECK(succeeded(run_child("--stop")));
}

// Decision 5 with ADR-0030's entry: a thread inside a host call gets no runtime signal and stops once
// the call returns.
TEST(a_thread_in_a_host_call_stops_when_the_call_returns) {
    CHECK(succeeded(run_child("--host-call")));
}

// Decision 5 with signals::resume: a runtime signal that an exit under way drops is sent again before the
// program's end, and after the end its thread stops; neither reports that the thread could not resume.
TEST(a_dropped_runtime_signal_is_sent_again_and_after_the_end_its_thread_stops) {
    const auto ended = run_child("--dropped-resume");
    CHECK(succeeded(ended));
    CHECK(ended.output.find("could not resume") == std::string::npos);
}

// Only a runtime signal that never arrives still ends the process, with the report.
TEST(a_runtime_signal_that_never_arrives_ends_the_process) {
    const auto ended = run_child("--resume-never", "", 127);
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 127);
    CHECK(ended.output.find("apkrt: a thread could not resume guest code") != std::string::npos);
}

/// The NDK fixture raw-dyn as an ELF file, or nothing while it is not built.
std::optional<std::vector<std::byte>> raw_dyn() {
    const auto path = fixtures / "raw-dyn";
    if (!std::filesystem::exists(path)) {
        std::printf("skipped: %s is not built, see tools/build-fixtures.sh\n", path.c_str());
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<std::byte> elf(std::filesystem::file_size(path));
    file.read(reinterpret_cast<char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
    return elf;
}

/// Sets the value of the dynamic entry `tag` in the ELF file `elf`; false when it has none.
bool set_dynamic_entry(std::vector<std::byte> &elf, std::int64_t tag, std::uint64_t value) {
    const auto read = [&](std::size_t offset, auto &field) { std::memcpy(&field, elf.data() + offset, sizeof field); };
    std::uint64_t header_offset = 0;
    std::uint16_t header_size = 0, header_count = 0;
    read(32, header_offset);  // e_phoff
    read(54, header_size);    // e_phentsize
    read(56, header_count);   // e_phnum
    for (std::size_t index = 0; index < header_count; ++index) {
        const std::size_t header = header_offset + index * header_size;
        std::uint32_t type = 0;
        std::uint64_t offset = 0, size = 0;
        read(header, type);
        read(header + 8, offset);  // p_offset
        read(header + 32, size);   // p_filesz
        if (type != 2) continue;   // PT_DYNAMIC
        for (std::size_t entry = offset; entry + 16 <= offset + size; entry += 16) {
            std::int64_t entry_tag = 0;
            read(entry, entry_tag);
            if (entry_tag != tag) continue;
            std::memcpy(elf.data() + entry + 8, &value, sizeof value);
            return true;
        }
    }
    return false;
}

// linker64 ends a program it cannot link with exit status 1: that end goes to the end handler too.
TEST(a_program_that_cannot_link_ends_through_the_end_handler) {
    const auto elf = raw_dyn();  // without libraw-dyn.so next to it
    if (!elf) return;
    const auto ended = run_child("--cannot-link", write_container("raw-dyn", *elf, scratch / "raw-dyn"));
    CHECK(succeeded(ended));
    CHECK(ended.output.find("CANNOT LINK EXECUTABLE") != std::string::npos);
}

// So does a program whose dynamic section the linker cannot read, now read on the program's thread.
TEST(a_program_with_a_broken_dynamic_section_ends_as_linker64_ends_it) {
    auto elf = raw_dyn();
    if (!elf) return;
    CHECK(set_dynamic_entry(*elf, 11 /* DT_SYMENT */, 16));
    const auto ended = run_child("--cannot-link", write_container("broken-dyn", *elf, scratch / "broken-dyn"));
    CHECK(succeeded(ended));
    CHECK(ended.output.find("CANNOT LINK EXECUTABLE") != std::string::npos);
    CHECK(ended.output.find("symbol entries of 16 bytes") != std::string::npos);
}

// Decision 5: without a session the program's end ends the process as before, after the report.
TEST(without_a_session_the_end_ends_the_process) {
    const auto ended = run_child("--no-session", "", 9);
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 9);
    CHECK(ended.report == "9\n");
}

}  // namespace

int main(int argc, char **argv) {
    program = argv[0];
    if (argc > 1 && std::string_view(argv[1]).starts_with("--")) run_case(argv[1], argc > 2 ? argv[2] : "");
    if (argc != 3) {
        std::fprintf(stderr, "usage: program_test <scratch directory> <fixtures directory>\n");
        return 2;
    }
    scratch = argv[1];
    fixtures = argv[2];
    std::filesystem::create_directories(scratch);
    return apk::testing::run_all();
}
