// Checks that blocking syscalls end as on Linux when signals come (ADR-0026): a futex wait or a sleep
// ends with EINTR at once when a signal is deliverable, however shortly before the wait it came, and a
// signal the thread blocks ends neither. Host threads call the dispatcher directly, as
// syscalls_test.cpp does; the guest threads are clone children that make their syscalls through the
// veneer's exact instructions (threads_probe.S, blocking_probe.S), and the test's own thread sends them
// signals like another thread of the program.
#include "signals.h"
#include "syscall_frame.h"
#include "syscalls.h"
#include "threads.h"

#include "check.h"

#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

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
void blocking_sigreturn();
}

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, asm-generic/signal.h, linux/futex.h,
// linux/time.h, errno.h.
constexpr std::uint64_t pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr std::uint64_t nr_futex = 98, nr_nanosleep = 101, nr_clock_nanosleep = 115, nr_tgkill = 131,
                        nr_rt_sigaction = 134, nr_rt_sigprocmask = 135, nr_rt_sigtimedwait = 137;
constexpr std::uint64_t futex_wait = 0, futex_wake = 1, futex_private = 128;
constexpr std::uint64_t everyone = 0x7fff'ffff;  // FUTEX_WAKE's count for all waiters
constexpr std::uint64_t sigusr1 = 10, sigusr2 = 12;
constexpr std::uint64_t sa_restart = 0x1000'0000, sa_restorer = 0x0400'0000;
constexpr std::uint64_t sig_block = 0, sig_unblock = 1;
constexpr std::uint64_t clock_monotonic = 1, timer_abstime = 1;
constexpr long eintr = 4, eagain = 11, etimedout = 110;

constexpr std::uint64_t bit(std::uint64_t signal) {
    return std::uint64_t{1} << (signal - 1);
}

/// struct timespec, the same on Linux arm64 and Darwin.
struct Timespec {
    std::int64_t seconds;
    std::int64_t nanoseconds;
};

std::int64_t nanoseconds_of(const Timespec &time) {
    return time.seconds * 1'000'000'000 + time.nanoseconds;
}

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

/// Linux CLOCK_MONOTONIC, as the runtime reads it.
std::int64_t now_ns() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

/// Waits up to `limit` for `condition`; whether it came true.
template <typename Condition>
bool within(std::chrono::nanoseconds limit, Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

/// Whether a host thread sleeps in the kernel, as a blocking syscall leaves it.
bool sleeping(pthread_t thread) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    return ::thread_info(::pthread_mach_thread_np(thread), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info),
                         &count) == KERN_SUCCESS &&
           info.run_state == TH_STATE_WAITING;
}

/// A syscall of the calling host thread, straight into the dispatcher. The syscall's exit leaves host
/// threads alone, so the result is the syscall's own.
long call(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a0;
    frame.x[1] = a1;
    frame.x[2] = a2;
    frame.x[3] = a3;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

/// What sending a signal does to the target thread's state, without the runtime signal: the signal
/// is pending, and the thread's signal word is raised.
void send_without_runtime_signal(apkrt::signals::ThreadSignals &state, std::uint64_t signal) {
    while (state.lock.test_and_set(std::memory_order_acquire)) {
    }
    state.pending.fetch_or(bit(signal));
    state.lock.clear(std::memory_order_release);
    apkrt::signals::notify(state);
}

void mark_pending(apkrt::signals::ThreadSignals &state) {
    state.pending.fetch_or(bit(sigusr1));
}

void leave_as_is(apkrt::signals::ThreadSignals &) {}

/// A blocking syscall on a host thread of its own, which the test watches.
class Waiter {
public:
    /// Makes `syscall` once `prepare` has set up the new thread's signal state.
    template <typename Prepare, typename Syscall>
    Waiter(Prepare prepare, Syscall syscall)
        : thread_([this, prepare, syscall] {
              auto &state = apkrt::signals::current();
              prepare(state);
              state_.store(&state);
              result_.store(syscall());
              finished_.store(true);
              // The signal state lives as long as the thread, so the thread waits for the test.
              while (!released_.load()) std::this_thread::sleep_for(1ms);
          }) {}
    Waiter(const Waiter &) = delete;
    Waiter &operator=(const Waiter &) = delete;
    ~Waiter() {
        released_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    apkrt::signals::ThreadSignals &state() {
        CHECK(within(2s, [&] { return state_.load() != nullptr; }));
        return *state_.load();
    }

    pthread_t host() { return thread_.native_handle(); }

    /// Whether the syscall has come back within `limit`.
    bool finishes_within(std::chrono::nanoseconds limit) {
        return within(limit, [&] { return finished_.load(); });
    }

    /// Whether the thread sleeps in its syscall within two seconds, and still a moment later: a thread
    /// that only waits for a lock on its way into the wait goes on at once.
    bool sleeps() {
        return within(2s, [&] {
            if (state_.load() == nullptr || finished_.load() || !sleeping(host())) return false;
            std::this_thread::sleep_for(2ms);
            return !finished_.load() && sleeping(host());
        });
    }

    /// The syscall's result. Where it did not end by itself, `unblock` ends it.
    template <typename Unblock>
    long result(Unblock unblock) {
        for (int attempt = 0; !finishes_within(20ms); ++attempt) {
            if (attempt == 500) {
                std::fprintf(stderr, "blocking_test: a syscall cannot be ended, giving up\n");
                std::_Exit(1);
            }
            unblock(*this);
        }
        released_.store(true);
        thread_.join();
        return result_.load();
    }

private:
    std::atomic<apkrt::signals::ThreadSignals *> state_{nullptr};
    std::atomic<bool> finished_{false};
    std::atomic<bool> released_{false};
    std::atomic<long> result_{0};
    std::thread thread_;  // last: it starts once the members above exist
};

/// Ends a futex wait on `word` that did not end by itself: with a wake.
auto by_waking(std::uint32_t *word) {
    return [word](Waiter &) { call(nr_futex, address(word), futex_wake | futex_private, everyone); };
}

/// Ends a sleep that did not end by itself: with a signal and the runtime signal, which interrupts
/// the host's sleep.
void by_signal(Waiter &waiter) {
    send_without_runtime_signal(waiter.state(), sigusr1);
    ::pthread_kill(waiter.host(), SIGURG);
}

// --- The wait itself ----------------------------------------------------------------------------

/// A wait whose condition raises the thread's signal word the first time it is asked: after the wait
/// has read the word, before it sleeps. Nothing else raises the word, so a wait that slept now would
/// sleep until `deadline`.
long wait_after_a_raise(std::int64_t deadline) {
    auto &state = apkrt::signals::current();
    int rounds = 0;
    const auto end = apkrt::sys::wait_until(
        state,
        [&] {
            if (++rounds == 1) apkrt::signals::notify(state);
            return rounds == 2;
        },
        deadline);
    return end == apkrt::sys::WaitEnd::done ? 0L : -1L;
}

TEST(a_raise_of_the_signal_word_after_the_wait_read_it_keeps_the_wait_from_sleeping) {
    const auto raise_again = [](Waiter &stuck) { apkrt::signals::notify(stuck.state()); };
    Waiter forever(leave_as_is, [] { return wait_after_a_raise(apkrt::sys::no_deadline); });
    CHECK(forever.finishes_within(2s));
    CHECK(forever.result(raise_again) == 0);
    Waiter with_deadline(leave_as_is, [] { return wait_after_a_raise(apkrt::sys::deadline_after(10'000'000'000)); });
    CHECK(with_deadline.finishes_within(2s));
    CHECK(with_deadline.result(raise_again) == 0);
}

// --- A signal that is deliverable before the wait -----------------------------------------------

TEST(a_futex_wait_ends_at_once_while_a_signal_is_deliverable) {
    alignas(4) static std::uint32_t word = 0;
    Waiter waiter(mark_pending, [] { return call(nr_futex, address(&word), futex_wait | futex_private, 0, 0); });
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result(by_waking(&word)) == -eintr);
}

TEST(a_futex_wait_on_another_value_answers_eagain_even_with_a_signal_pending) {
    // Linux compares first: only a wait that would sleep ends with EINTR.
    auto &state = apkrt::signals::current();
    mark_pending(state);
    alignas(4) std::uint32_t word = 1;
    CHECK(call(nr_futex, address(&word), futex_wait | futex_private, 0, 0) == -eagain);
    state.pending.fetch_and(~bit(sigusr1));
}

TEST(a_sleep_ends_at_once_while_a_signal_is_deliverable) {
    static const Timespec ten_seconds{10, 0};
    static Timespec left{-1, -1};
    Waiter waiter(mark_pending, [] { return call(nr_nanosleep, address(&ten_seconds), address(&left)); });
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result(by_signal) == -eintr);
    CHECK(nanoseconds_of(left) > 9'000'000'000 && nanoseconds_of(left) <= 10'000'000'000);  // what is left
}

TEST(clock_nanosleep_ends_at_once_while_a_signal_is_deliverable) {
    static const Timespec ten_seconds{10, 0};
    static Timespec left{-1, -1};
    Waiter relative(mark_pending, [] {
        return call(nr_clock_nanosleep, clock_monotonic, 0, address(&ten_seconds), address(&left));
    });
    CHECK(relative.finishes_within(2s));
    CHECK(relative.result(by_signal) == -eintr);
    CHECK(nanoseconds_of(left) > 9'000'000'000 && nanoseconds_of(left) <= 10'000'000'000);

    // Until a time: EINTR, and nothing is written.
    static Timespec until{};
    const auto target = now_ns() + 10'000'000'000;
    until = {target / 1'000'000'000, target % 1'000'000'000};
    static Timespec untouched{-1, -1};
    Waiter absolute(mark_pending, [] {
        return call(nr_clock_nanosleep, clock_monotonic, timer_abstime, address(&until), address(&untouched));
    });
    CHECK(absolute.finishes_within(2s));
    CHECK(absolute.result(by_signal) == -eintr);
    CHECK(untouched.seconds == -1 && untouched.nanoseconds == -1);
}

TEST(a_sleep_whose_time_is_up_ends_as_slept_even_with_a_signal_pending) {
    // Linux ends a sleep whose time is up as done, before it looks at signals.
    auto &state = apkrt::signals::current();
    mark_pending(state);
    const Timespec zero{0, 0};
    Timespec left{-1, -1};
    CHECK(call(nr_nanosleep, address(&zero), address(&left)) == 0);
    CHECK(left.seconds == -1 && left.nanoseconds == -1);  // nothing written
    state.pending.fetch_and(~bit(sigusr1));
}

TEST(sigtimedwait_without_time_answers_eagain_even_with_another_signal_pending) {
    // With a timeout of zero Linux does not sleep, so the other pending signal interrupts nothing.
    auto &state = apkrt::signals::current();
    mark_pending(state);
    const std::uint64_t wanted = bit(sigusr2);
    const Timespec zero{0, 0};
    CHECK(call(nr_rt_sigtimedwait, address(&wanted), 0, address(&zero), 8) == -eagain);
    state.pending.fetch_and(~bit(sigusr1));
}

// --- A signal sent during the wait --------------------------------------------------------------

TEST(a_signal_sent_during_a_futex_wait_ends_it_without_the_runtime_signal) {
    alignas(4) static std::uint32_t word = 0;
    Waiter waiter(leave_as_is, [] { return call(nr_futex, address(&word), futex_wait | futex_private, 0, 0); });
    CHECK(waiter.sleeps());
    send_without_runtime_signal(waiter.state(), sigusr1);
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result(by_waking(&word)) == -eintr);
    // It is no longer queued: a wake finds nobody.
    CHECK(call(nr_futex, address(&word), futex_wake | futex_private, everyone) == 0);
}

TEST(a_signal_sent_during_a_sleep_ends_it_without_the_runtime_signal) {
    static const Timespec ten_seconds{10, 0};
    Waiter waiter(leave_as_is, [] { return call(nr_nanosleep, address(&ten_seconds), 0); });
    CHECK(waiter.sleeps());
    send_without_runtime_signal(waiter.state(), sigusr1);
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result(by_signal) == -eintr);
}

// --- Guest threads ------------------------------------------------------------------------------

std::atomic<int> handled[64];

/// The guest handler of SIGUSR1 and SIGUSR2: counts.
void on_signal(int signal) {
    if (signal > 0 && signal < 64) handled[signal].fetch_add(1);
}

/// SIGUSR1 and SIGUSR2 go to on_signal, with SA_RESTART.
void install_handlers() {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{address(&on_signal), sa_restart | sa_restorer, address(&blocking_sigreturn), 0};
    for (const auto signal : {sigusr1, sigusr2}) {
        CHECK(probe_syscall(nr_rt_sigaction, signal, address(&action), 0, 8) == 0);
    }
}

constexpr std::size_t stack_size = 256 * 1024;

/// A clone child that runs `function(argument)` as guest code on a stack of its own, the way
/// bionic's pthread_create starts a thread.
class Child {
public:
    Child(void (*function)(void *), void *argument)
        : stack_(::mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)) {
        CHECK(stack_ != MAP_FAILED);
        tid_ = static_cast<std::int32_t>(probe_clone(pthread_flags, static_cast<char *>(stack_) + stack_size,
                                                     &tid_word_, nullptr, &tid_word_, function, argument));
        CHECK(tid_ > 0);
    }
    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    ~Child() {
        join();
        ::munmap(stack_, stack_size);
    }

    std::uint64_t tid() const { return static_cast<std::uint64_t>(tid_); }

    /// The host thread it runs on.
    pthread_t host() const {
        pthread_t found = nullptr;
        CHECK(within(2s, [&] {
            apkrt::threads::find(tid_, [&](apkrt::threads::GuestThread &thread) { found = thread.signals.host; });
            return found != nullptr;
        }));
        return found;
    }

    /// pthread_join's wait: until the kernel clears the thread ID word at the thread's exit. A thread
    /// that does not end within ten seconds cannot be joined: the test gives up.
    void join() {
        const Timespec slice{0, 100'000'000};
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (int value; (value = __atomic_load_n(&tid_word_, __ATOMIC_ACQUIRE)) != 0;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "blocking_test: a guest thread does not end, giving up\n");
                std::_Exit(1);
            }
            const long result = probe_syscall(nr_futex, address(&tid_word_), futex_wait,
                                              static_cast<std::uint64_t>(value), address(&slice));
            CHECK(result == 0 || result == -eagain || result == -etimedout);
        }
    }

private:
    void *stack_;
    int tid_word_ = 0;
    std::int32_t tid_ = 0;
};

/// The rounds of the stress tests: the test's thread starts a round and sends SIGUSR1, and the child
/// enters its wait at a random moment up to 6 µs after it saw the round begin.
struct Rounds {
    bool sleep = false;         // nanosleep, else a futex wait on `word`
    std::atomic<int> round{0};  // -1 ends the child
    std::atomic<int> done{0};   // the last round whose syscall came back
    alignas(4) std::uint32_t word = 0;
};

void wait_in_rounds(void *argument) {
    auto &rounds = *static_cast<Rounds *>(argument);
    std::uint64_t random = 0x2545'f491'4f6c'dd1d;
    for (int next = 1;; ++next) {
        for (int round; (round = rounds.round.load()) != next;) {
            if (round < 0) return;
        }
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        for (const auto until = now_ns() + static_cast<std::int64_t>(random % 6000); now_ns() < until;) {
        }
        if (rounds.sleep) {
            const Timespec ten_seconds{10, 0};
            probe_syscall(nr_nanosleep, address(&ten_seconds), 0);
        } else {
            probe_syscall(nr_futex, address(&rounds.word), futex_wait | futex_private, 0, 0);
        }
        rounds.done.store(next);
    }
}

/// Runs `count` rounds; the first in which the handler did not run within 500 ms, or 0.
int first_lost_signal(Rounds &rounds, std::uint64_t tid, int count) {
    const auto pid = static_cast<std::uint64_t>(::getpid());
    for (int round = 1; round <= count; ++round) {
        __atomic_store_n(&rounds.word, 0, __ATOMIC_SEQ_CST);
        const int before = handled[sigusr1].load();
        rounds.round.store(round);
        probe_syscall(nr_tgkill, pid, tid, sigusr1);
        const bool reached = within(500ms, [&] { return handled[sigusr1].load() > before; });
        // The wait ends by its futex word; a sleep, whose signal may have come before it, by more signals.
        if (!rounds.sleep) {
            __atomic_store_n(&rounds.word, 1, __ATOMIC_SEQ_CST);
            probe_syscall(nr_futex, address(&rounds.word), futex_wake | futex_private, everyone);
        }
        const bool ended = within(5s, [&] {
            if (rounds.done.load() >= round) return true;
            if (rounds.sleep) probe_syscall(nr_tgkill, pid, tid, sigusr2);
            std::this_thread::sleep_for(200us);
            return rounds.done.load() >= round;
        });
        if (!reached || !ended) {
            std::fprintf(stderr, "round %d: handler %s, wait %s\n", round, reached ? "ran" : "did not run within 500 ms",
                         ended ? "ended" : "did not end");
            return round;
        }
    }
    return 0;
}

TEST(every_signal_reaches_a_thread_on_its_way_into_a_futex_wait) {
    static Rounds rounds;
    Child child(wait_in_rounds, &rounds);
    CHECK(first_lost_signal(rounds, child.tid(), 3000) == 0);
    rounds.round.store(-1);
}

TEST(every_signal_reaches_a_thread_on_its_way_into_a_sleep) {
    static Rounds rounds;
    rounds.sleep = true;
    Child child(wait_in_rounds, &rounds);
    CHECK(first_lost_signal(rounds, child.tid(), 1000) == 0);
    rounds.round.store(-1);
}

/// The guest thread of the next test: a futex wait and a sleep with SIGUSR2 blocked.
struct Blocked {
    alignas(4) std::uint32_t word = 0;
    std::atomic<int> phase{0};  // 1: the futex wait, 2: the sleep, 3: past both
    std::atomic<long> futex_result{1};
    std::atomic<long> sleep_result{1};
    std::atomic<std::int64_t> slept_ns{0};
};

void wait_with_sigusr2_blocked(void *argument) {
    auto &blocked = *static_cast<Blocked *>(argument);
    const std::uint64_t set = bit(sigusr2);
    probe_syscall(nr_rt_sigprocmask, sig_block, address(&set), 0, 8);
    blocked.phase.store(1);
    blocked.futex_result.store(probe_syscall(nr_futex, address(&blocked.word), futex_wait | futex_private, 0, 0));
    blocked.phase.store(2);
    const Timespec quarter_second{0, 250'000'000};
    const auto start = now_ns();
    blocked.sleep_result.store(probe_syscall(nr_nanosleep, address(&quarter_second), 0));
    blocked.slept_ns.store(now_ns() - start);
    blocked.phase.store(3);
    probe_syscall(nr_rt_sigprocmask, sig_unblock, address(&set), 0, 8);  // the pending SIGUSR2 arrives here
}

TEST(a_signal_the_thread_blocks_ends_neither_a_futex_wait_nor_a_sleep) {
    static Blocked blocked;
    const int before = handled[sigusr2].load();
    Child child(wait_with_sigusr2_blocked, &blocked);
    const auto host = child.host();
    const auto send_three = [&] {
        for (int time = 0; time < 3; ++time) {
            CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), child.tid(), sigusr2) == 0);
            std::this_thread::sleep_for(5ms);
        }
    };
    CHECK(within(2s, [&] { return blocked.phase.load() == 1 && sleeping(host); }));
    send_three();
    __atomic_store_n(&blocked.word, 1, __ATOMIC_SEQ_CST);
    CHECK(within(2s, [&] {
        probe_syscall(nr_futex, address(&blocked.word), futex_wake | futex_private, everyone);
        return blocked.phase.load() >= 2;
    }));
    CHECK(within(2s, [&] { return blocked.phase.load() == 3 || (blocked.phase.load() == 2 && sleeping(host)); }));
    send_three();
    child.join();
    CHECK(blocked.futex_result.load() == 0);
    CHECK(blocked.sleep_result.load() == 0);
    CHECK(blocked.slept_ns.load() >= 250'000'000);
    CHECK(handled[sigusr2].load() == before + 1);  // once, when the thread unblocked it
}

// --- The futex queue ----------------------------------------------------------------------------

TEST(futex_wake_wakes_as_many_as_asked_and_counts_them) {
    alignas(4) static std::uint32_t word = 0;
    const auto wait = [] { return call(nr_futex, address(&word), futex_wait | futex_private, 0, 0); };
    Waiter first(leave_as_is, wait), second(leave_as_is, wait), third(leave_as_is, wait);
    CHECK(first.sleeps() && second.sleeps() && third.sleeps());
    CHECK(call(nr_futex, address(&word), futex_wake | futex_private, 2) == 2);
    CHECK(call(nr_futex, address(&word), futex_wake | futex_private, everyone) == 1);
    for (auto *waiter : {&first, &second, &third}) CHECK(waiter->result(by_waking(&word)) == 0);
}

TEST(futex_wake_leaves_the_waiters_of_other_words_alone) {
    // Enough words that some share the waiter's bucket, whatever the hash.
    alignas(4) static std::uint32_t words[1024] = {};
    Waiter waiter(leave_as_is, [] { return call(nr_futex, address(&words[0]), futex_wait | futex_private, 0, 0); });
    CHECK(waiter.sleeps());
    long woken = 0;
    for (auto &word : words) {
        if (&word != &words[0]) woken += call(nr_futex, address(&word), futex_wake | futex_private, everyone);
    }
    CHECK(woken == 0);
    CHECK(waiter.sleeps());
    CHECK(call(nr_futex, address(&words[0]), futex_wake | futex_private, everyone) == 1);
    CHECK(waiter.result(by_waking(&words[0])) == 0);
}

TEST(a_futex_wake_right_after_a_change_of_the_word_reaches_every_waiter) {
    // The waker raises a generation word and wakes everyone, as a condition variable's broadcast does,
    // then waits until each waiter has seen the generation. A waiter that compared the word just before
    // the change and queued itself just after the wake would sleep for good.
    constexpr int waiters = 4;
    constexpr std::uint32_t generations = 20'000;
    alignas(4) static std::uint32_t generation = 0;
    static std::atomic<int> seen{0};
    static std::atomic<bool> stop{false};
    const auto follow = [] {
        for (std::uint32_t last = 0; last < generations;) {
            const std::uint32_t now = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
            if (now != last) {
                last = now;
                seen.fetch_add(1);
                continue;
            }
            if (stop.load()) return -1L;
            const long result = call(nr_futex, address(&generation), futex_wait | futex_private, last, 0);
            if (result != 0 && result != -eagain) return result;
        }
        return 0L;
    };
    Waiter first(leave_as_is, follow), second(leave_as_is, follow), third(leave_as_is, follow),
        fourth(leave_as_is, follow);
    bool stalled = false;
    for (std::uint32_t next = 1; next <= generations && !stalled; ++next) {
        __atomic_store_n(&generation, next, __ATOMIC_RELEASE);
        call(nr_futex, address(&generation), futex_wake | futex_private, everyone);
        const int expected = waiters * static_cast<int>(next);
        stalled = !within(500ms, [&] { return seen.load() == expected; });
    }
    CHECK(!stalled);
    stop.store(stalled);
    for (auto *waiter : {&first, &second, &third, &fourth}) {
        const long result = waiter->result(by_waking(&generation));
        if (!stalled) CHECK(result == 0);
    }
}

TEST(a_futex_wait_that_timed_out_is_no_longer_queued) {
    alignas(4) std::uint32_t word = 0;
    const Timespec twenty_ms{0, 20'000'000};
    CHECK(call(nr_futex, address(&word), futex_wait | futex_private, 0, address(&twenty_ms)) == -etimedout);
    CHECK(call(nr_futex, address(&word), futex_wake | futex_private, everyone) == 0);
}

TEST(futex_wake_counts_exactly_the_waiters_that_return_0_while_signals_come_too) {
    // A signal, and a wake up to 3 µs later: whichever the waiter finds first, a waiter that a wake
    // counted returns 0, and one that returned EINTR was counted by none.
    constexpr int rounds = 2000;
    static std::atomic<int> round{0};
    static std::atomic<int> done{0};
    static std::atomic<long> result{0};
    alignas(4) static std::uint32_t word = 0;
    Waiter waiter(leave_as_is, [] {
        auto &state = apkrt::signals::current();
        for (int next = 1; next <= rounds; ++next) {
            while (round.load() != next) {
                if (round.load() < 0) return 0L;
                std::this_thread::yield();
            }
            result.store(call(nr_futex, address(&word), futex_wait | futex_private, 0, 0));
            state.pending.fetch_and(~bit(sigusr1));  // a host thread takes no signals: gone for the next round
            done.store(next);
        }
        return 0L;
    });
    auto &state = waiter.state();
    std::uint64_t random = 0x853c'49e6'748f'ea9b;
    int mismatches = 0;
    for (int next = 1; next <= rounds; ++next) {
        __atomic_store_n(&word, 0, __ATOMIC_SEQ_CST);
        round.store(next);
        CHECK(within(2s, [&] { return sleeping(waiter.host()); }));
        send_without_runtime_signal(state, sigusr1);
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        for (const auto until = now_ns() + static_cast<std::int64_t>(random % 3000); now_ns() < until;) {
        }
        const long woken = call(nr_futex, address(&word), futex_wake | futex_private, 1);
        if (!within(2s, [&] { return done.load() == next; })) {
            CHECK(false);
            round.store(-1);
            break;
        }
        if ((woken == 1) != (result.load() == 0)) ++mismatches;
    }
    CHECK(mismatches == 0);
    CHECK(waiter.result(by_waking(&word)) == 0);
}

TEST(a_futex_waiter_woken_before_a_signal_came_returns_0) {
    alignas(4) static std::uint32_t word = 0;
    Waiter waiter(leave_as_is, [] { return call(nr_futex, address(&word), futex_wait | futex_private, 0, 0); });
    CHECK(waiter.sleeps());
    CHECK(call(nr_futex, address(&word), futex_wake | futex_private, 1) == 1);
    send_without_runtime_signal(waiter.state(), sigusr1);  // as on Linux, the wake that came first wins
    CHECK(waiter.result(by_waking(&word)) == 0);
}

}  // namespace

int main() {
    // The test's own thread is the program's first thread: the process lives as long as it does.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    // The runtime signal's handler, which the waits of host threads need before any clone installs it.
    apkrt::signals::install();
    install_handlers();
    return apk::testing::run_all();
}
