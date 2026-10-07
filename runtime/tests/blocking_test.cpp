// Checks that blocking syscalls end as on Linux when signals come (ADR-0026, ADR-0027): a futex wait, a
// sleep or a blocking read or write on a pipe ends with EINTR at once when a signal is deliverable,
// however shortly before the wait it came, and a signal the thread blocks ends none of them. Host
// threads call the dispatcher directly, as
// syscalls_test.cpp does; the guest threads are clone children that make their syscalls through the
// veneer's exact instructions (threads_probe.S, blocking_probe.S), and the test's own thread sends them
// signals like another thread of the program.
#include "descriptors.h"
#include "signals.h"
#include "syscall_frame.h"
#include "syscalls.h"
#include "threads.h"

#include "check.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
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
long probe_syscall6(std::uint64_t number, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                    std::uint64_t a4, std::uint64_t a5);
void blocking_sigreturn();
}

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, asm-generic/signal.h, linux/futex.h,
// linux/time.h, errno.h.
constexpr std::uint64_t pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr std::uint64_t nr_eventfd2 = 19, nr_epoll_create1 = 20, nr_epoll_ctl = 21, nr_epoll_pwait = 22, nr_fcntl = 25,
                        nr_ioctl = 29, nr_close = 57, nr_pipe2 = 59, nr_read = 63, nr_write = 64, nr_writev = 66,
                        nr_pselect6 = 72, nr_ppoll = 73, nr_futex = 98, nr_nanosleep = 101, nr_clock_nanosleep = 115,
                        nr_tgkill = 131, nr_rt_sigaction = 134, nr_rt_sigprocmask = 135, nr_rt_sigtimedwait = 137,
                        nr_gettid = 178, nr_epoll_pwait2 = 441;
constexpr std::uint64_t f_setfl = 4, o_nonblock = 04000, fionread = 0x541b;
constexpr std::uint64_t futex_wait = 0, futex_wake = 1, futex_private = 128;
constexpr std::uint64_t everyone = 0x7fff'ffff;  // FUTEX_WAKE's count for all waiters
constexpr std::uint64_t sigusr1 = 10, sigusr2 = 12, sigpipe = 13;
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

TEST(a_wait_on_a_descriptor_the_kqueue_refuses_polls_instead_of_spinning) {
    // The kqueue refuses /dev/null, which no wait of the guest watches (ADR-0027, decision 5). Should a
    // wait meet such a descriptor all the same, it looks again every millisecond instead of spinning.
    const int null = ::open("/dev/null", O_RDONLY);
    CHECK(null >= 0);
    apkrt::sys::Watched watched{null, EVFILT_READ};
    int rounds = 0;
    const auto start = now_ns();
    const auto end = apkrt::sys::wait_for_descriptors(
        apkrt::signals::current(), &watched, 1,
        [&] {
            ++rounds;
            return now_ns() - start >= 20'000'000;
        },
        apkrt::sys::no_deadline);
    CHECK(end == apkrt::sys::WaitEnd::done);
    CHECK(rounds <= 60);                         // about 20; a spin makes thousands
    CHECK(now_ns() - start < 500'000'000);  // and a look every millisecond, not every second
    ::close(null);
}

TEST(a_wait_whose_kqueue_fails_polls_instead_of_spinning) {
    // A kevent that fails for good, here on a kqueue closed behind the thread's back, never happens;
    // should it, the wait looks again every millisecond instead of spinning.
    int ends[2] = {-1, -1};
    CHECK(::pipe(ends) == 0);
    Waiter waiter(leave_as_is, [&] {
        auto &state = apkrt::signals::current();
        const int gone = ::kqueue();
        ::close(gone);
        state.wait_queue.store(gone);
        apkrt::sys::Watched watched{ends[0], EVFILT_READ};
        int rounds = 0;
        const auto start = now_ns();
        apkrt::sys::wait_for_descriptors(
            state, &watched, 1,
            [&] {
                ++rounds;
                return now_ns() - start >= 20'000'000;
            },
            apkrt::sys::no_deadline);
        state.wait_queue.store(-1);  // nothing for the end of the thread to close
        return static_cast<long>(rounds);
    });
    CHECK(waiter.result([](Waiter &) {}) <= 60);  // about 20; a spin makes thousands
    for (const int end : ends) ::close(end);
}

TEST(a_wait_on_descriptors_makes_its_kqueue_before_it_first_checks_for_signals) {
    // Else a signal sent between the check and the making of the kqueue would trigger nothing there,
    // and the wait would sleep through it.
    int ends[2] = {-1, -1};
    CHECK(::pipe(ends) == 0);
    Waiter waiter(leave_as_is, [&] {
        auto &state = apkrt::signals::current();  // a new thread's: no kqueue yet
        apkrt::sys::Watched watched{ends[0], EVFILT_READ};
        const auto end = apkrt::sys::wait_for_descriptors(
            state, &watched, 1,
            [&] {
                mark_pending(state);  // a signal that came while the wait looked at its condition
                return false;
            },
            apkrt::sys::no_deadline);
        return end == apkrt::sys::WaitEnd::signal && state.wait_queue.load() >= 0 ? 0L : -1L;
    });
    CHECK(waiter.result([](Waiter &stuck) { apkrt::signals::notify(stuck.state()); }) == 0);
    for (const int end : ends) ::close(end);
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

// --- Blocking reads and writes on pipes (ADR-0027) ------------------------------------------------

/// A pipe of the guest's, from pipe2 with `flags`; both ends close with it.
struct Pipe {
    int read_end = -1;
    int write_end = -1;

    explicit Pipe(std::uint64_t flags = 0) {
        int ends[2] = {-1, -1};
        CHECK(call(nr_pipe2, address(ends), flags) == 0);
        read_end = ends[0];
        write_end = ends[1];
    }
    ~Pipe() {
        call(nr_close, static_cast<std::uint64_t>(read_end));
        call(nr_close, static_cast<std::uint64_t>(write_end));
    }
    Pipe(const Pipe &) = delete;
    Pipe &operator=(const Pipe &) = delete;
};

long read_from(int fd, void *buffer, std::size_t size) {
    return call(nr_read, static_cast<std::uint64_t>(fd), address(buffer), size);
}

long write_to(int fd, const void *buffer, std::size_t size) {
    return call(nr_write, static_cast<std::uint64_t>(fd), address(buffer), size);
}

/// Fills `pipe` until not a byte more fits, with the guest's O_NONBLOCK on its write end for the time;
/// how many bytes it holds then.
std::size_t fill(const Pipe &pipe) {
    CHECK(call(nr_fcntl, static_cast<std::uint64_t>(pipe.write_end), f_setfl, o_nonblock) == 0);
    static const char chunk[4096] = {};
    std::size_t held = 0;
    for (const std::size_t size : {sizeof chunk, std::size_t{1}}) {
        for (long written; (written = write_to(pipe.write_end, chunk, size)) > 0;) {
            held += static_cast<std::size_t>(written);
        }
    }
    CHECK(call(nr_fcntl, static_cast<std::uint64_t>(pipe.write_end), f_setfl, 0) == 0);
    return held;
}

/// Takes what `pipe` holds, with the guest's O_NONBLOCK on its read end from now on; how many bytes.
std::size_t drain(const Pipe &pipe) {
    CHECK(call(nr_fcntl, static_cast<std::uint64_t>(pipe.read_end), f_setfl, o_nonblock) == 0);
    static char chunk[65536];
    std::size_t taken = 0;
    for (long got; (got = read_from(pipe.read_end, chunk, sizeof chunk)) > 0;) taken += static_cast<std::size_t>(got);
    return taken;
}

/// Ends a read of `pipe` that did not end by itself: with a byte to read.
auto by_writing_to(const Pipe &pipe) {
    return [&pipe](Waiter &) { write_to(pipe.write_end, "x", 1); };
}

/// Ends a write to `pipe` that did not end by itself: with room to write.
auto by_draining(const Pipe &pipe) {
    return [&pipe](Waiter &) { drain(pipe); };
}

/// The host descriptor behind a guest number.
int host_of(int fd) {
    const auto description = apkrt::descriptors::get(fd);
    return description != nullptr ? description->host() : -1;
}

/// Whether the thread's kqueue holds no filter for reading `host` any more; takes it out if it does.
bool holds_no_filter(apkrt::signals::ThreadSignals &state, int host) {
    const int queue = state.wait_queue.load();
    if (queue < 0) return true;
    struct kevent change, receipt;
    EV_SET(&change, host, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
    return ::kevent(queue, &change, 1, &receipt, 1, nullptr) == 1 && receipt.data == ENOENT;
}

TEST(a_blocking_pipe_read_ends_at_once_while_a_signal_is_deliverable) {
    Pipe pipe;
    Waiter waiter(mark_pending, [&] {
        char byte = 0;
        return read_from(pipe.read_end, &byte, 1);
    });
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result(by_writing_to(pipe)) == -eintr);
}

TEST(a_blocking_pipe_write_ends_at_once_while_a_signal_is_deliverable) {
    Pipe pipe;
    const std::size_t held = fill(pipe);
    Waiter waiter(mark_pending, [&] { return write_to(pipe.write_end, "more", 4); });
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result(by_draining(pipe)) == -eintr);
    CHECK(drain(pipe) == held);  // not a byte more
}

TEST(a_signal_sent_during_a_blocking_pipe_read_ends_it_without_the_runtime_signal) {
    Pipe pipe;
    Waiter waiter(leave_as_is, [&] {
        char byte = 0;
        return read_from(pipe.read_end, &byte, 1);
    });
    CHECK(waiter.sleeps());
    send_without_runtime_signal(waiter.state(), sigusr1);
    CHECK(waiter.finishes_within(2s));
    CHECK(holds_no_filter(waiter.state(), host_of(pipe.read_end)));  // the wait took its filter out again
    CHECK(waiter.result(by_writing_to(pipe)) == -eintr);
}

TEST(a_wait_leaves_no_filter_behind_that_wakes_the_next_wait_in_vain) {
    // The first pipe keeps a byte while the thread waits for the second: a filter left for it would
    // wake that wait again and again.
    Pipe first, second;
    Waiter waiter(leave_as_is, [&] {
        char byte = 0;
        read_from(first.read_end, &byte, 1);
        return read_from(second.read_end, &byte, 1);
    });
    CHECK(waiter.sleeps());
    write_to(first.write_end, "ab", 2);
    int left = 0;
    CHECK(within(2s, [&] {
        call(nr_ioctl, static_cast<std::uint64_t>(first.read_end), fionread, address(&left));
        return left == 1;
    }));
    CHECK(waiter.sleeps());
    CHECK(holds_no_filter(waiter.state(), host_of(first.read_end)));
    CHECK(waiter.result(by_writing_to(second)) == 1);
}

TEST(a_blocking_writev_writes_every_vector_in_order) {
    // More than a pipe holds, in three vectors and an empty one: the write goes on from wherever the
    // pipe was full, while the test takes what arrives.
    static char parts[3][100 * 1024];
    std::string expected;
    for (int part = 0; part < 3; ++part) {
        for (std::size_t index = 0; index < sizeof parts[part]; ++index) {
            parts[part][index] = static_cast<char>(part * 31 + static_cast<int>(index % 251));
        }
        expected.append(parts[part], sizeof parts[part]);
    }
    const iovec vectors[4] = {{parts[0], sizeof parts[0]}, {parts[1], 0}, {parts[1], sizeof parts[1]},
                              {parts[2], sizeof parts[2]}};
    Pipe pipe;
    Waiter waiter(leave_as_is, [&] { return call(nr_writev, static_cast<std::uint64_t>(pipe.write_end), address(vectors), 4); });
    CHECK(call(nr_fcntl, static_cast<std::uint64_t>(pipe.read_end), f_setfl, o_nonblock) == 0);
    std::string received;
    static char chunk[65536];
    CHECK(within(5s, [&] {
        for (long got; (got = read_from(pipe.read_end, chunk, sizeof chunk)) > 0;) {
            received.append(chunk, static_cast<std::size_t>(got));
        }
        return received.size() == expected.size();
    }));
    CHECK(waiter.result(by_draining(pipe)) == static_cast<long>(expected.size()));
    CHECK(received == expected);
}

TEST(a_threads_kqueue_goes_with_the_thread) {
    const auto open_descriptors = [] {
        int count = 0;
        for (int fd = 0; fd < 4096; ++fd) count += ::fcntl(fd, F_GETFD) != -1 ? 1 : 0;
        return count;
    };
    Pipe pipe;
    const int before = open_descriptors();
    for (int round = 0; round < 8; ++round) {
        Waiter waiter(leave_as_is, [&] {
            char byte = 0;
            return read_from(pipe.read_end, &byte, 1);
        });
        CHECK(waiter.sleeps());  // in its wait: it has its kqueue
        CHECK(waiter.result(by_writing_to(pipe)) == 1);
    }
    CHECK(open_descriptors() == before);
}

TEST(a_signal_sent_during_a_blocking_pipe_write_ends_it_without_the_runtime_signal) {
    Pipe pipe;
    fill(pipe);
    Waiter waiter(leave_as_is, [&] { return write_to(pipe.write_end, "more", 4); });
    CHECK(waiter.sleeps());
    send_without_runtime_signal(waiter.state(), sigusr1);
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result(by_draining(pipe)) == -eintr);
}

TEST(a_blocking_write_that_a_signal_ends_after_some_bytes_returns_their_count) {
    // As on Linux: EINTR only when not a byte was written.
    static const char big[256 * 1024] = {};
    Pipe pipe;
    Waiter waiter(leave_as_is, [&] { return write_to(pipe.write_end, big, sizeof big); });
    CHECK(waiter.sleeps());
    send_without_runtime_signal(waiter.state(), sigusr1);
    CHECK(waiter.finishes_within(2s));
    const long written = waiter.result(by_draining(pipe));
    CHECK(written > 0 && written < static_cast<long>(sizeof big));
    CHECK(drain(pipe) == static_cast<std::size_t>(written));
}

// --- The waits of eventfd, epoll, ppoll and pselect6 (ADR-0027, decisions 8 to 10) ----------------------

/// struct epoll_event on arm64 and struct pollfd.
struct EpollEvent {
    std::uint32_t events;
    std::uint64_t data;
};
struct PollFd {
    std::int32_t fd;
    std::int16_t events;
    std::int16_t revents;
};
constexpr std::uint32_t epollin = 0x1;
constexpr std::int16_t pollin = 0x1;

/// An eventfd at 0, an empty pipe, and an epoll with the pipe's read end: what the blocking waits of
/// eventfd, epoll, ppoll and pselect6 wait for here, until a write comes.
struct Events {
    int counter = -1;
    Pipe pipe;
    int epoll = -1;

    Events() {
        counter = static_cast<int>(call(nr_eventfd2, 0, 0));
        epoll = static_cast<int>(call(nr_epoll_create1, 0));
        const EpollEvent event{epollin, 0};
        CHECK(counter >= 0 && epoll >= 0);
        CHECK(call(nr_epoll_ctl, static_cast<std::uint64_t>(epoll), 1, static_cast<std::uint64_t>(pipe.read_end),
                   address(&event)) == 0);
    }
    ~Events() {
        call(nr_close, static_cast<std::uint64_t>(counter));
        call(nr_close, static_cast<std::uint64_t>(epoll));
    }
    Events(const Events &) = delete;
    Events &operator=(const Events &) = delete;

    // Each wait without a timeout; the fifth and sixth arguments of the frame are 0: no mask.
    long read_counter() const {
        std::uint64_t value = 0;
        return call(nr_read, static_cast<std::uint64_t>(counter), address(&value), sizeof value);
    }
    long wait_on_epoll() const {
        EpollEvent events[1];
        return call(nr_epoll_pwait, static_cast<std::uint64_t>(epoll), address(events), 1, static_cast<std::uint64_t>(-1));
    }
    long poll_pipe() const {
        PollFd fd{pipe.read_end, pollin, 0};
        return call(nr_ppoll, address(&fd), 1, 0, 0);
    }
    long select_pipe() const {
        std::uint64_t set[16] = {};
        set[pipe.read_end / 64] |= std::uint64_t{1} << (pipe.read_end % 64);
        return call(nr_pselect6, static_cast<std::uint64_t>(pipe.read_end + 1), address(set), 0, 0);
    }
};

/// Ends a wait on `events` that did not end by itself: with a count for the eventfd and a byte for the pipe.
auto by_writing_to(const Events &events) {
    return [&events](Waiter &) {
        const std::uint64_t one = 1;
        call(nr_write, static_cast<std::uint64_t>(events.counter), address(&one), sizeof one);
        write_to(events.pipe.write_end, "x", 1);
    };
}

TEST(the_waits_of_eventfd_epoll_ppoll_and_pselect6_end_at_once_while_a_signal_is_deliverable) {
    const Events events;
    const std::function<long()> waits[] = {[&] { return events.read_counter(); }, [&] { return events.wait_on_epoll(); },
                                           [&] { return events.poll_pipe(); }, [&] { return events.select_pipe(); }};
    for (const auto &wait : waits) {
        Waiter waiter(mark_pending, wait);
        CHECK(waiter.finishes_within(2s));
        CHECK(waiter.result(by_writing_to(events)) == -eintr);
    }
}

TEST(a_signal_sent_during_the_waits_of_eventfd_epoll_ppoll_and_pselect6_ends_them_without_the_runtime_signal) {
    const Events events;
    const std::function<long()> waits[] = {[&] { return events.read_counter(); }, [&] { return events.wait_on_epoll(); },
                                           [&] { return events.poll_pipe(); }, [&] { return events.select_pipe(); }};
    // What each wait watches in the thread's kqueue, and must take out again.
    const int watched[] = {host_of(events.counter), host_of(events.epoll), host_of(events.pipe.read_end),
                           host_of(events.pipe.read_end)};
    for (int index = 0; index < 4; ++index) {
        Waiter waiter(leave_as_is, waits[index]);
        CHECK(waiter.sleeps());
        send_without_runtime_signal(waiter.state(), sigusr1);
        CHECK(waiter.finishes_within(2s));
        CHECK(holds_no_filter(waiter.state(), watched[index]));
        CHECK(waiter.result(by_writing_to(events)) == -eintr);
    }
}

TEST(the_waits_of_eventfd_epoll_ppoll_and_pselect6_end_with_a_write_from_another_thread) {
    const Events events;
    const std::function<long()> waits[] = {[&] { return events.read_counter(); }, [&] { return events.wait_on_epoll(); },
                                           [&] { return events.poll_pipe(); }, [&] { return events.select_pipe(); }};
    const long expected[] = {8, 1, 1, 1};
    for (int index = 0; index < 4; ++index) {
        Waiter waiter(leave_as_is, waits[index]);
        CHECK(waiter.sleeps());
        const std::uint64_t one = 1;
        CHECK(call(nr_write, static_cast<std::uint64_t>(events.counter), address(&one), sizeof one) == 8);
        CHECK(write_to(events.pipe.write_end, "w", 1) == 1);
        CHECK(waiter.finishes_within(2s));
        CHECK(waiter.result(by_writing_to(events)) == expected[index]);
        // What the next wait waits for again: the eventfd at 0 and an empty pipe.
        std::uint64_t value = 0;
        CHECK(call(nr_fcntl, static_cast<std::uint64_t>(events.counter), f_setfl, o_nonblock) == 0);
        call(nr_read, static_cast<std::uint64_t>(events.counter), address(&value), sizeof value);
        CHECK(call(nr_fcntl, static_cast<std::uint64_t>(events.counter), f_setfl, 0) == 0);
        drain(events.pipe);
        CHECK(call(nr_fcntl, static_cast<std::uint64_t>(events.pipe.read_end), f_setfl, 0) == 0);
    }
}

TEST(two_threads_waiting_on_one_epoll_do_not_take_each_others_wake_ups) {
    const Events events;
    Waiter first(leave_as_is, [&] { return events.wait_on_epoll(); });
    Waiter second(leave_as_is, [&] { return events.wait_on_epoll(); });
    CHECK(first.sleeps() && second.sleeps());
    send_without_runtime_signal(first.state(), sigusr1);
    CHECK(first.finishes_within(2s));
    CHECK(second.sleeps());
    CHECK(first.result(by_writing_to(events)) == -eintr);
    CHECK(write_to(events.pipe.write_end, "e", 1) == 1);
    CHECK(second.finishes_within(2s));
    CHECK(second.result(by_writing_to(events)) == 1);
}

TEST(a_one_shot_interest_that_reported_leaves_no_filter_that_wakes_the_wait_in_vain) {
    // Two numbers of one pipe end: the first asks for EPOLLIN, the second only for what always comes,
    // both one-shot. Once the first has reported the byte, which stays, nothing asks for it any more:
    // a wait sleeps until the writer goes, and the hangup is the second one's alone.
    constexpr std::uint64_t nr_dup = 23;
    constexpr std::uint32_t epolloneshot = 1u << 30;
    Pipe pipe;
    const auto epoll = static_cast<std::uint64_t>(call(nr_epoll_create1, 0));
    const auto copy = static_cast<std::uint64_t>(call(nr_dup, static_cast<std::uint64_t>(pipe.read_end)));
    const EpollEvent first{epollin | epolloneshot, 1}, second{epolloneshot, 2};
    CHECK(call(nr_epoll_ctl, epoll, 1, static_cast<std::uint64_t>(pipe.read_end), address(&first)) == 0);
    CHECK(call(nr_epoll_ctl, epoll, 1, copy, address(&second)) == 0);
    CHECK(write_to(pipe.write_end, "b", 1) == 1);
    EpollEvent events[2] = {};
    CHECK(call(nr_epoll_pwait, epoll, address(events), 2, 0) == 1 && events[0].data == 1);
    Waiter waiter(leave_as_is, [&] {
        EpollEvent got[2] = {};
        const long count = call(nr_epoll_pwait, epoll, address(got), 2, static_cast<std::uint64_t>(-1));
        return count == 1 && got[0].data == 2 && got[0].events == 0x10 ? 1L : -count - 1;  // EPOLLHUP
    });
    CHECK(waiter.sleeps());
    call(nr_close, static_cast<std::uint64_t>(pipe.write_end));
    pipe.write_end = -1;
    CHECK(waiter.finishes_within(2s));
    CHECK(waiter.result([](Waiter &stuck) { send_without_runtime_signal(stuck.state(), sigusr1); }) == 1);
    call(nr_close, copy);
    call(nr_close, epoll);
}

TEST(a_poll_whose_kqueue_cannot_tell_reports_no_hangup) {
    // Should the thread's kqueue fail, every filter counts as ready (ADR-0027 b), for the call to find
    // out, but no pipe's other end as gone.
    Pipe pipe;
    Waiter waiter(leave_as_is, [&] {
        auto &state = apkrt::signals::current();
        const int gone = ::kqueue();
        ::close(gone);
        state.wait_queue.store(gone);
        PollFd fd{pipe.read_end, 0, 0};
        Timespec twenty_ms{0, 20'000'000};
        const long result = call(nr_ppoll, address(&fd), 1, address(&twenty_ms), 0);
        state.wait_queue.store(-1);  // nothing for the end of the thread to close
        return result == 0 && fd.revents == 0 ? 0L : -1L;
    });
    CHECK(waiter.result([](Waiter &) {}) == 0);
}

// --- Guest threads ------------------------------------------------------------------------------

std::atomic<int> handled[64];

/// The guest handler of SIGUSR1 and SIGUSR2: counts.
void on_signal(int signal) {
    if (signal > 0 && signal < 64) handled[signal].fetch_add(1);
}

/// SIGUSR1, SIGUSR2 and SIGPIPE go to on_signal, with SA_RESTART.
void install_handlers() {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{address(&on_signal), sa_restart | sa_restorer, address(&blocking_sigreturn), 0};
    for (const auto signal : {sigusr1, sigusr2, sigpipe}) {
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

/// An epoll with the read end of a pipe of its own, for guest threads.
struct EpollOnPipe {
    int epoll = -1;
    int pipe[2] = {-1, -1};

    void open() {
        epoll = static_cast<int>(probe_syscall(nr_epoll_create1, 0));
        CHECK(epoll >= 0 && probe_syscall(nr_pipe2, address(pipe), 0) == 0);
        const EpollEvent event{epollin, 0};
        CHECK(probe_syscall(nr_epoll_ctl, static_cast<std::uint64_t>(epoll), 1, static_cast<std::uint64_t>(pipe[0]),
                            address(&event)) == 0);
    }
    void close() const {
        for (const int fd : {epoll, pipe[0], pipe[1]}) probe_syscall(nr_close, static_cast<std::uint64_t>(fd));
    }
};

/// The rounds of the stress tests: the test's thread starts a round and sends SIGUSR1, and the child
/// enters its wait at a random moment up to 6 µs after it saw the round begin.
struct Rounds {
    enum class Wait { futex, sleep, pipe_read, epoll };
    // A futex wait on `word`, nanosleep, a read of a byte from `pipe`, or epoll_pwait on `epoll`, which
    // watches the pipe, and then the read of the byte.
    Wait wait = Wait::futex;
    std::atomic<int> round{0};  // -1 ends the child
    std::atomic<int> done{0};   // the last round whose syscall came back
    alignas(4) std::uint32_t word = 0;
    int pipe[2] = {-1, -1};  // the guest's pipe2, for pipe_read and epoll
    int epoll = -1;
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
        switch (rounds.wait) {
            case Rounds::Wait::futex:
                probe_syscall(nr_futex, address(&rounds.word), futex_wait | futex_private, 0, 0);
                break;
            case Rounds::Wait::sleep: {
                const Timespec ten_seconds{10, 0};
                probe_syscall(nr_nanosleep, address(&ten_seconds), 0);
                break;
            }
            case Rounds::Wait::pipe_read: {
                char byte = 0;
                probe_syscall(nr_read, static_cast<std::uint64_t>(rounds.pipe[0]), address(&byte), 1);
                break;
            }
            case Rounds::Wait::epoll: {
                EpollEvent events[1];
                probe_syscall6(nr_epoll_pwait, static_cast<std::uint64_t>(rounds.epoll), address(events), 1,
                               static_cast<std::uint64_t>(-1), 0, 8);
                char byte = 0;
                probe_syscall(nr_read, static_cast<std::uint64_t>(rounds.pipe[0]), address(&byte), 1);
                break;
            }
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
        // A futex wait ends by its word, a read and an epoll wait by the byte; a sleep, whose signal may
        // have come before it, by more signals.
        if (rounds.wait == Rounds::Wait::futex) {
            __atomic_store_n(&rounds.word, 1, __ATOMIC_SEQ_CST);
            probe_syscall(nr_futex, address(&rounds.word), futex_wake | futex_private, everyone);
        } else if (rounds.wait == Rounds::Wait::pipe_read || rounds.wait == Rounds::Wait::epoll) {
            probe_syscall(nr_write, static_cast<std::uint64_t>(rounds.pipe[1]), address("x"), 1);
        }
        const bool ended = within(5s, [&] {
            if (rounds.done.load() >= round) return true;
            if (rounds.wait == Rounds::Wait::sleep) probe_syscall(nr_tgkill, pid, tid, sigusr2);
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
    rounds.wait = Rounds::Wait::sleep;
    Child child(wait_in_rounds, &rounds);
    CHECK(first_lost_signal(rounds, child.tid(), 1000) == 0);
    rounds.round.store(-1);
}

TEST(every_signal_reaches_a_thread_on_its_way_into_a_blocking_pipe_read) {
    static Rounds rounds;
    rounds.wait = Rounds::Wait::pipe_read;
    CHECK(probe_syscall(nr_pipe2, address(rounds.pipe), 0) == 0);
    {
        Child child(wait_in_rounds, &rounds);
        CHECK(first_lost_signal(rounds, child.tid(), 3000) == 0);
        rounds.round.store(-1);
    }
    for (const int end : rounds.pipe) probe_syscall(nr_close, static_cast<std::uint64_t>(end));
}

TEST(every_signal_reaches_a_thread_on_its_way_into_an_epoll_wait) {
    static Rounds rounds;
    rounds.wait = Rounds::Wait::epoll;
    static EpollOnPipe events;
    events.open();
    rounds.pipe[0] = events.pipe[0];
    rounds.pipe[1] = events.pipe[1];
    rounds.epoll = events.epoll;
    {
        Child child(wait_in_rounds, &rounds);
        CHECK(first_lost_signal(rounds, child.tid(), 3000) == 0);
        rounds.round.store(-1);
    }
    events.close();
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

/// The guest thread of the next test: a blocking read and a blocking write on a pipe with SIGUSR2
/// blocked.
struct BlockedTransfers {
    int pipe[2] = {-1, -1};
    std::atomic<int> phase{0};  // 1: the read, 2: the write, 3: past both
    std::atomic<long> read_result{1};
    std::atomic<long> write_result{1};
    char received[16] = {};
};

constexpr std::size_t big_write = 256 * 1024;  // more than a pipe holds
char big_buffer[big_write];                    // a pattern that the test finds again at the other end

void transfer_with_sigusr2_blocked(void *argument) {
    auto &transfers = *static_cast<BlockedTransfers *>(argument);
    const std::uint64_t set = bit(sigusr2);
    probe_syscall(nr_rt_sigprocmask, sig_block, address(&set), 0, 8);
    transfers.phase.store(1);
    transfers.read_result.store(probe_syscall(nr_read, static_cast<std::uint64_t>(transfers.pipe[0]),
                                              address(transfers.received), sizeof transfers.received));
    transfers.phase.store(2);
    transfers.write_result.store(
        probe_syscall(nr_write, static_cast<std::uint64_t>(transfers.pipe[1]), address(big_buffer), big_write));
    transfers.phase.store(3);
    probe_syscall(nr_rt_sigprocmask, sig_unblock, address(&set), 0, 8);  // the pending SIGUSR2 arrives here
}

TEST(a_signal_the_thread_blocks_ends_neither_a_blocking_read_nor_a_blocking_write) {
    static BlockedTransfers transfers;
    for (std::size_t index = 0; index < big_write; ++index) {
        big_buffer[index] = static_cast<char>(index % 253 + index / 65536);
    }
    CHECK(probe_syscall(nr_pipe2, address(transfers.pipe), 0) == 0);
    const auto read_end = static_cast<std::uint64_t>(transfers.pipe[0]);
    const auto write_end = static_cast<std::uint64_t>(transfers.pipe[1]);
    const int before = handled[sigusr2].load();
    Child child(transfer_with_sigusr2_blocked, &transfers);
    const auto host = child.host();
    const auto send_three = [&] {
        for (int time = 0; time < 3; ++time) {
            CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), child.tid(), sigusr2) == 0);
            std::this_thread::sleep_for(5ms);
        }
    };
    CHECK(within(2s, [&] { return transfers.phase.load() == 1 && sleeping(host); }));
    send_three();
    // Still in the read: the data goes to it. A read that ended already gets none, lest the test's own
    // write wait for good behind the thread's in a full pipe.
    const bool still_reading = transfers.phase.load() == 1;
    CHECK(still_reading);
    if (still_reading) CHECK(probe_syscall(nr_write, write_end, address("data"), 4) == 4);
    CHECK(within(2s, [&] { return transfers.phase.load() == 3 || (transfers.phase.load() == 2 && sleeping(host)); }));
    send_three();
    // The test takes what the write puts into the pipe, until the write is past.
    CHECK(probe_syscall(nr_fcntl, read_end, f_setfl, o_nonblock) == 0);
    static char chunk[65536];
    std::size_t drained = 0;
    bool intact = true;  // the bytes arrive as the write had them
    CHECK(within(5s, [&] {
        const bool past = transfers.phase.load() == 3;
        for (long got; (got = probe_syscall(nr_read, read_end, address(chunk), sizeof chunk)) > 0;) {
            const auto size = static_cast<std::size_t>(got);
            intact = intact && drained + size <= big_write && std::memcmp(chunk, big_buffer + drained, size) == 0;
            drained += size;
        }
        return past;
    }));
    child.join();
    CHECK(transfers.read_result.load() == 4 && std::memcmp(transfers.received, "data", 4) == 0);
    CHECK(transfers.write_result.load() == static_cast<long>(big_write));
    CHECK(drained == big_write && intact);
    CHECK(handled[sigusr2].load() == before + 1);  // once, when the thread unblocked it
    probe_syscall(nr_close, read_end);
    probe_syscall(nr_close, write_end);
}

/// The guest thread of the next test: a write of more than a pipe holds, whose reader goes away.
struct BrokenPipe {
    int pipe[2] = {-1, -1};
    std::atomic<int> phase{0};  // 1: the write, 2: past it
    std::atomic<long> result{1};
};

void write_into_a_pipe_that_breaks(void *argument) {
    auto &broken = *static_cast<BrokenPipe *>(argument);
    broken.phase.store(1);
    broken.result.store(
        probe_syscall(nr_write, static_cast<std::uint64_t>(broken.pipe[1]), address(big_buffer), big_write));
    broken.phase.store(2);
}

TEST(a_write_whose_reader_goes_after_some_bytes_returns_their_count_and_raises_sigpipe) {
    // As Linux's pipe_write: SIGPIPE comes with the count as well, not only with EPIPE.
    static BrokenPipe broken;
    CHECK(probe_syscall(nr_pipe2, address(broken.pipe), 0) == 0);
    const int before = handled[sigpipe].load();
    Child child(write_into_a_pipe_that_breaks, &broken);
    const auto host = child.host();
    CHECK(within(2s, [&] { return broken.phase.load() == 1 && sleeping(host); }));
    probe_syscall(nr_close, static_cast<std::uint64_t>(broken.pipe[0]));  // the reader goes
    child.join();
    CHECK(broken.result.load() > 0 && broken.result.load() < static_cast<long>(big_write));
    CHECK(handled[sigpipe].load() == before + 1);
    probe_syscall(nr_close, static_cast<std::uint64_t>(broken.pipe[1]));
}

TEST(a_socket_write_whose_peer_goes_after_some_bytes_returns_their_count_without_sigpipe) {
    // Unlike pipe_write, unix_stream_sendmsg sends SIGPIPE only when nothing went out (ADR-0054 step a).
    constexpr std::uint64_t nr_socketpair = 199, af_unix = 1, sock_stream = 1;
    static BrokenPipe broken;  // pipe[1] writes, pipe[0] is its peer
    CHECK(probe_syscall(nr_socketpair, af_unix, sock_stream, 0, address(broken.pipe)) == 0);
    const int before = handled[sigpipe].load();
    Child child(write_into_a_pipe_that_breaks, &broken);
    const auto host = child.host();
    CHECK(within(2s, [&] { return broken.phase.load() == 1 && sleeping(host); }));
    probe_syscall(nr_close, static_cast<std::uint64_t>(broken.pipe[0]));  // the peer goes
    child.join();
    CHECK(broken.result.load() > 0 && broken.result.load() < static_cast<long>(big_write));
    CHECK(handled[sigpipe].load() == before);
    probe_syscall(nr_close, static_cast<std::uint64_t>(broken.pipe[1]));
}

/// The guest thread of the next test: epoll_pwait, epoll_pwait2, ppoll and pselect6 in turn, each for up
/// to 5 s on the pipe, which stays empty.
struct FourWaits {
    EpollOnPipe events;
    std::atomic<int> phase{0};  // the wait under way, 1 to 4; 5 past all of them
    std::atomic<long> results[4] = {};
};

void wait_four_ways(void *argument) {
    auto &waits = *static_cast<FourWaits *>(argument);
    const auto epoll = static_cast<std::uint64_t>(waits.events.epoll);
    const int read_end = waits.events.pipe[0];
    for (int wait = 0; wait < 4; ++wait) {
        Timespec five_seconds{5, 0};
        EpollEvent events[1];
        PollFd fd{read_end, pollin, 0};
        std::uint64_t set[16] = {};
        set[read_end / 64] |= std::uint64_t{1} << (read_end % 64);
        waits.phase.store(wait + 1);
        long result;
        switch (wait) {
            case 0: result = probe_syscall6(nr_epoll_pwait, epoll, address(events), 1, 5000, 0, 8); break;
            case 1:
                result = probe_syscall6(nr_epoll_pwait2, epoll, address(events), 1, address(&five_seconds), 0, 8);
                break;
            case 2: result = probe_syscall6(nr_ppoll, address(&fd), 1, address(&five_seconds), 0, 8, 0); break;
            default:
                result = probe_syscall6(nr_pselect6, static_cast<std::uint64_t>(read_end + 1), address(set), 0, 0,
                                        address(&five_seconds), 0);
                break;
        }
        waits.results[wait].store(result);
    }
    waits.phase.store(5);
}

TEST(a_handler_with_sa_restart_restarts_none_of_the_waits_of_epoll_ppoll_and_pselect6) {
    // As on Linux: epoll_pwait and epoll_pwait2 answer EINTR, ppoll and pselect6 ERESTARTNOHAND, which
    // a handler turns into EINTR, SA_RESTART or not.
    static FourWaits waits;
    waits.events.open();
    Child child(wait_four_ways, &waits);
    const auto host = child.host();
    for (int wait = 1; wait <= 4; ++wait) {
        CHECK(within(2s, [&] { return waits.phase.load() == wait && sleeping(host); }));
        const int before = handled[sigusr1].load();
        CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), child.tid(), sigusr1) == 0);
        CHECK(within(2s, [&] { return waits.phase.load() > wait; }));  // the signal ended it, not its time
        CHECK(handled[sigusr1].load() == before + 1);
    }
    child.join();
    // A restart would show as the first argument: the probe's veneer has no svc to go back to.
    for (const auto &result : waits.results) CHECK(result.load() == -eintr);
    waits.events.close();
}

/// The guest thread of the next test: epoll_pwait and ppoll of 150 ms each, with SIGUSR2 blocked.
struct BlockedWaits {
    EpollOnPipe events;
    std::atomic<int> phase{0};  // 1: epoll_pwait, 2: ppoll, 3: past both
    std::atomic<long> results[2] = {1, 1};
    std::atomic<std::int64_t> waited_ns[2] = {};
};

void wait_on_events_with_sigusr2_blocked(void *argument) {
    auto &waits = *static_cast<BlockedWaits *>(argument);
    const std::uint64_t set = bit(sigusr2);
    probe_syscall(nr_rt_sigprocmask, sig_block, address(&set), 0, 8);
    for (int wait = 0; wait < 2; ++wait) {
        Timespec timeout{0, 150'000'000};
        EpollEvent events[1];
        PollFd fd{waits.events.pipe[0], pollin, 0};
        waits.phase.store(wait + 1);
        const auto start = now_ns();
        waits.results[wait].store(
            wait == 0 ? probe_syscall6(nr_epoll_pwait, static_cast<std::uint64_t>(waits.events.epoll), address(events), 1,
                                       150, 0, 8)
                      : probe_syscall6(nr_ppoll, address(&fd), 1, address(&timeout), 0, 8, 0));
        waits.waited_ns[wait].store(now_ns() - start);
    }
    waits.phase.store(3);
    probe_syscall(nr_rt_sigprocmask, sig_unblock, address(&set), 0, 8);  // the pending SIGUSR2 arrives here
}

TEST(a_signal_the_thread_blocks_ends_neither_an_epoll_wait_nor_a_ppoll) {
    static BlockedWaits waits;
    waits.events.open();
    const int before = handled[sigusr2].load();
    Child child(wait_on_events_with_sigusr2_blocked, &waits);
    const auto host = child.host();
    for (int wait = 1; wait <= 2; ++wait) {
        CHECK(within(2s, [&] { return waits.phase.load() == wait && sleeping(host); }));
        for (int time = 0; time < 3; ++time) {
            CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), child.tid(), sigusr2) == 0);
            std::this_thread::sleep_for(5ms);
        }
    }
    child.join();
    for (int wait = 0; wait < 2; ++wait) {
        CHECK(waits.results[wait].load() == 0);
        CHECK(waits.waited_ns[wait].load() >= 150'000'000);
    }
    CHECK(handled[sigusr2].load() == before + 1);  // once, when the thread unblocked it
    waits.events.close();
}

/// The guest thread of the next tests: SIGUSR2 blocked and pending, then a ppoll of `timeout` whose mask
/// lets it in, and the thread's mask after it.
struct MaskedWait {
    Timespec timeout{5, 0};
    std::atomic<long> result{1};
    std::atomic<std::uint64_t> mask_after{0};
};

void wait_with_a_mask_that_lets_sigusr2_in(void *argument) {
    auto &wait = *static_cast<MaskedWait *>(argument);
    const std::uint64_t set = bit(sigusr2), open = 0;
    probe_syscall(nr_rt_sigprocmask, sig_block, address(&set), 0, 8);
    const auto tid = static_cast<std::uint64_t>(probe_syscall(nr_gettid, 0));
    probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), tid, sigusr2);
    Timespec timeout = wait.timeout;
    wait.result.store(probe_syscall6(nr_ppoll, 0, 0, address(&timeout), address(&open), 8, 0));
    std::uint64_t mask = 0;
    probe_syscall(nr_rt_sigprocmask, sig_block, 0, address(&mask), 8);
    wait.mask_after.store(mask);
    probe_syscall(nr_rt_sigprocmask, sig_unblock, address(&set), 0, 8);
}

TEST(ppolls_mask_lets_a_pending_signal_in_and_the_threads_own_mask_holds_again_after_the_handler) {
    static MaskedWait wait;
    const int before = handled[sigusr2].load();
    {
        const Child child(wait_with_a_mask_that_lets_sigusr2_in, &wait);
    }
    CHECK(wait.result.load() == -eintr);
    CHECK(handled[sigusr2].load() == before + 1);
    CHECK((wait.mask_after.load() & bit(sigusr2)) != 0);
}

TEST(a_signal_that_ends_ppolls_wait_but_is_ignored_leaves_the_threads_own_mask) {
    // SIG_IGN before it is sent, so that the blocked signal stays pending, and is dropped when delivered:
    // no handler's frame restores the mask then. Linux restarts the wait instead of EINTR (a remaining
    // difference, ADR-0027).
    const struct {
        std::uint64_t handler, flags, restorer, mask;
    } ignore{1, 0, 0, 0};  // SIG_IGN
    CHECK(probe_syscall(nr_rt_sigaction, sigusr2, address(&ignore), 0, 8) == 0);
    static MaskedWait wait;
    wait.timeout = {0, 100'000'000};
    {
        const Child child(wait_with_a_mask_that_lets_sigusr2_in, &wait);
    }
    install_handlers();
    CHECK(wait.result.load() == -eintr);
    CHECK((wait.mask_after.load() & bit(sigusr2)) != 0);
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

TEST(a_futex_wake_raises_only_the_signal_word) {
    // A futex wait sleeps on the word (ADR-0027, decision 6): the wake triggers nothing in the thread's
    // kqueue, which the thread's next wait on a descriptor would wake up for in vain.
    Pipe pipe;
    alignas(4) static std::uint32_t word = 0;
    Waiter waiter(leave_as_is, [&] {
        char byte = 0;
        read_from(pipe.read_end, &byte, 1);  // a wait on a descriptor: the thread has its kqueue from here on
        return call(nr_futex, address(&word), futex_wait | futex_private, 0, 0);
    });
    CHECK(waiter.sleeps());
    write_to(pipe.write_end, "x", 1);
    CHECK(within(2s, [&] { return call(nr_futex, address(&word), futex_wake | futex_private, 1) == 1; }));
    CHECK(waiter.finishes_within(2s));
    const int queue = waiter.state().wait_queue.load();
    struct kevent event;
    const timespec now{0, 0};
    CHECK(queue >= 0 && ::kevent(queue, nullptr, 0, &event, 1, &now) == 0);
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
