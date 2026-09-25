// futex of the Linux syscall layer (ADR-0009, ADR-0026). The runtime keeps the waiters itself, in
// buckets by address, and a waiter sleeps on its thread's signal word in wait_until (syscalls.h), so a
// signal sent at any moment ends the wait. FUTEX_WAKE takes waiters off their bucket's list and raises
// their words. As on Linux, FUTEX_WAIT compares the word under its bucket's lock and FUTEX_WAKE takes
// the same lock: a wake that follows a change of the word finds every waiter that saw the old value.
#include "signals.h"
#include "syscalls.h"

#include <os/lock.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace apkrt::sys {
namespace {

namespace linux_futex {
constexpr int wait = 0;
constexpr int wake = 1;
constexpr int wait_bitset = 9;
constexpr int wake_bitset = 10;
constexpr int private_flag = 128;
constexpr int clock_realtime = 256;
constexpr std::uint32_t bitset_match_any = 0xffff'ffff;
}  // namespace linux_futex

/// A thread in FUTEX_WAIT, on its own stack. It is in its bucket's list until a wake takes it off
/// or its wait ends by a signal or the deadline.
struct Waiter {
    std::uint64_t address;
    signals::ThreadSignals *thread;
    std::atomic<bool> woken{false};     // taken off by a wake, under the lock
    std::atomic<bool> released{false};  // the waker is done with it and its thread
    Waiter *previous = nullptr;
    Waiter *next = nullptr;
};

/// The waiters whose address hashes here, in the order they came.
struct Bucket {
    os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    Waiter *first = nullptr;
    Waiter *last = nullptr;
};

Bucket buckets[256];

Bucket &bucket_of(std::uint64_t address) {
    // Fibonacci hashing of the word's index; the top 8 bits pick the bucket.
    return buckets[((address >> 2) * 0x9e37'79b9'7f4a'7c15) >> 56];
}

class Locked {
public:
    explicit Locked(Bucket &bucket) : bucket_(bucket) { ::os_unfair_lock_lock(&bucket_.lock); }
    ~Locked() { ::os_unfair_lock_unlock(&bucket_.lock); }
    Locked(const Locked &) = delete;
    Locked &operator=(const Locked &) = delete;

private:
    Bucket &bucket_;
};

void append(Bucket &bucket, Waiter &waiter) {
    waiter.previous = bucket.last;
    waiter.next = nullptr;
    (bucket.last != nullptr ? bucket.last->next : bucket.first) = &waiter;
    bucket.last = &waiter;
}

void remove(Bucket &bucket, Waiter &waiter) {
    (waiter.previous != nullptr ? waiter.previous->next : bucket.first) = waiter.next;
    (waiter.next != nullptr ? waiter.next->previous : bucket.last) = waiter.previous;
}

long futex_wait(std::uint32_t *word, std::uint32_t expected, std::int64_t deadline) {
    auto &state = signals::current();
    const auto address = reinterpret_cast<std::uint64_t>(word);
    auto &bucket = bucket_of(address);
    Waiter self{address, &state};
    {
        const Locked locked(bucket);
        if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected) return -linux_errno::eagain;
        append(bucket, self);
    }
    if (wait_until(state, [&] { return self.woken.load(std::memory_order_acquire); }, deadline) != WaitEnd::done) {
        const Locked locked(bucket);
        if (!self.woken.load(std::memory_order_relaxed)) {  // as on Linux, a wake wins over a timeout or a signal
            remove(bucket, self);
            if (deadline != no_deadline && monotonic_now() >= deadline) return -linux_errno::etimedout;
            return -linux_errno::eintr;
        }
    }
    // Woken: the waker still raises this thread's signal word, so the thread must not end before.
    while (!self.released.load(std::memory_order_acquire)) std::this_thread::yield();
    return 0;
}

}  // namespace

long futex_wake(std::uint32_t *word, int count, bool) {
    const auto address = reinterpret_cast<std::uint64_t>(word);
    auto &bucket = bucket_of(address);
    Waiter *taken = nullptr;  // off the list, linked through next
    long woken = 0;
    {
        const Locked locked(bucket);
        for (Waiter *waiter = bucket.first; waiter != nullptr && woken < count;) {
            Waiter *const next = waiter->next;
            if (waiter->address == address) {
                remove(bucket, *waiter);
                waiter->woken.store(true, std::memory_order_release);
                waiter->next = taken;
                taken = waiter;
                ++woken;
            }
            waiter = next;
        }
    }
    // Raised after the lock, as with Linux's wake queue: a woken thread need not wait for the lock. Only
    // the word: the waiter sleeps on it, not in its kqueue (ADR-0027).
    while (taken != nullptr) {
        Waiter *const next = taken->next;
        signals::raise_signal_word(*taken->thread);
        taken->released.store(true, std::memory_order_release);  // the last access: the waiter may go
        taken = next;
    }
    return woken;
}

long sys_futex(std::uint64_t address, int operation, std::uint32_t value, std::uint64_t timeout_address,
               std::uint32_t bitset) {
    if (address % 4 != 0) return -linux_errno::einval;
    auto *word = as_pointer<std::uint32_t>(address);
    const int command = operation & ~(linux_futex::private_flag | linux_futex::clock_realtime);

    switch (command) {
        case linux_futex::wait:
        case linux_futex::wait_bitset: {
            if (command == linux_futex::wait_bitset && bitset != linux_futex::bitset_match_any) break;
            std::int64_t deadline = no_deadline;
            if (timeout_address != 0) {
                const auto &time = *as_pointer<const timespec>(timeout_address);
                if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1'000'000'000) return -linux_errno::einval;
                if (command == linux_futex::wait) {
                    deadline = deadline_after(nanoseconds_of(time));  // relative
                } else {
                    // Absolute, on CLOCK_MONOTONIC or with FUTEX_CLOCK_REALTIME on CLOCK_REALTIME.
                    const auto clock = (operation & linux_futex::clock_realtime) ? CLOCK_REALTIME : CLOCK_UPTIME_RAW;
                    const auto left = nanoseconds_of(time) - static_cast<std::int64_t>(clock_gettime_nsec_np(clock));
                    deadline = deadline_after(std::max<std::int64_t>(left, 0));
                }
            }
            return futex_wait(word, value, deadline);
        }
        case linux_futex::wake:
        case linux_futex::wake_bitset:
            if (command == linux_futex::wake_bitset && bitset != linux_futex::bitset_match_any) break;
            return futex_wake(word, static_cast<int>(std::min<std::uint32_t>(value, INT32_MAX)),
                              (operation & linux_futex::private_flag) == 0);
        default: break;
    }
    // Requeue, wake-op, priority inheritance and bitsets other than "any" follow when a program needs them.
    char message[80];
    const int length = std::snprintf(message, sizeof message, "apkrt: futex operation %#x is not supported yet\n",
                                     static_cast<unsigned>(operation));
    if (length > 0) static_cast<void>(::write(STDERR_FILENO, message, static_cast<std::size_t>(length)));
    return -linux_errno::enosys;
}

}  // namespace apkrt::sys
