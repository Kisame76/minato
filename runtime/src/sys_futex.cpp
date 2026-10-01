// futex of the Linux syscall layer (ADR-0009, ADR-0026). The runtime keeps the waiters itself, in
// buckets by address, and a waiter sleeps on its thread's signal word in wait_until (syscalls.h), so a
// signal sent at any moment ends the wait. FUTEX_WAKE takes waiters off their bucket's list and raises
// their words. As on Linux, FUTEX_WAIT compares the word under its bucket's lock and FUTEX_WAKE takes
// the same lock: a wake that follows a change of the word finds every waiter that saw the old value.
// FUTEX_REQUEUE moves waiters to another word's bucket (ADR-0033, decision 8), so a waiter whose wait
// ends by itself looks up which bucket it is in by then.
#include "signals.h"
#include "syscalls.h"

#include <os/lock.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace apkrt::sys {
namespace {

namespace linux_futex {
constexpr int wait = 0;
constexpr int wake = 1;
constexpr int requeue = 3;
constexpr int wait_bitset = 9;
constexpr int wake_bitset = 10;
constexpr int private_flag = 128;
constexpr int clock_realtime = 256;
constexpr std::uint32_t bitset_match_any = 0xffff'ffff;
}  // namespace linux_futex

struct Bucket;

/// A thread in FUTEX_WAIT, on its own stack. It is in its bucket's list until a wake takes it off
/// or its wait ends by a signal or the deadline. A requeue changes its address and bucket while it
/// holds the locks of both buckets.
struct Waiter {
    std::uint64_t address;
    signals::ThreadSignals *thread;
    std::atomic<Bucket *> bucket;
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
    /// Takes over a lock the caller holds already.
    Locked(Bucket &bucket, std::adopt_lock_t) : bucket_(bucket) {}
    ~Locked() { ::os_unfair_lock_unlock(&bucket_.lock); }
    Locked(const Locked &) = delete;
    Locked &operator=(const Locked &) = delete;

private:
    Bucket &bucket_;
};

/// The locks of two buckets, which may be the same one, taken in the order of the table: two requeues
/// between the same words in opposite directions cannot wait for each other.
class LockedPair {
public:
    LockedPair(Bucket &one, Bucket &other) : first_(std::min(&one, &other)), second_(std::max(&one, &other)) {
        ::os_unfair_lock_lock(&first_->lock);
        if (second_ != first_) ::os_unfair_lock_lock(&second_->lock);
    }
    ~LockedPair() {
        if (second_ != first_) ::os_unfair_lock_unlock(&second_->lock);
        ::os_unfair_lock_unlock(&first_->lock);
    }
    LockedPair(const LockedPair &) = delete;
    LockedPair &operator=(const LockedPair &) = delete;

private:
    Bucket *const first_;
    Bucket *const second_;
};

/// Locks the bucket `waiter` is in now. A requeue moves a waiter only while it holds that bucket's
/// lock, so the waiter stays where it is found under it.
Bucket &lock_bucket_of(const Waiter &waiter) {
    Bucket *bucket = waiter.bucket.load(std::memory_order_acquire);
    while (true) {
        ::os_unfair_lock_lock(&bucket->lock);
        Bucket *const now = waiter.bucket.load(std::memory_order_acquire);
        if (now == bucket) return *bucket;
        ::os_unfair_lock_unlock(&bucket->lock);
        bucket = now;
    }
}

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
    Waiter self{address, &state, &bucket};
    {
        const Locked locked(bucket);
        if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected) return -linux_errno::eagain;
        append(bucket, self);
    }
    if (wait_until(state, [&] { return self.woken.load(std::memory_order_acquire); }, deadline) != WaitEnd::done) {
        auto &current = lock_bucket_of(self);  // a requeue may have moved it
        const Locked locked(current, std::adopt_lock);
        if (!self.woken.load(std::memory_order_relaxed)) {  // as on Linux, a wake wins over a timeout or a signal
            remove(current, self);
            if (deadline != no_deadline && monotonic_now() >= deadline) return -linux_errno::etimedout;
            return -linux_errno::eintr;
        }
    }
    // Woken: the waker still raises this thread's signal word, so the thread must not end before.
    while (!self.released.load(std::memory_order_acquire)) std::this_thread::yield();
    return 0;
}

/// Raises the signal words of the woken waiters `taken`, linked through next. After the lock, as with
/// Linux's wake queue: a woken thread need not wait for the lock. Only the word: the waiter sleeps on
/// it, not in its kqueue (ADR-0027).
void release(Waiter *taken) {
    while (taken != nullptr) {
        Waiter *const next = taken->next;
        signals::raise_signal_word(*taken->thread);
        taken->released.store(true, std::memory_order_release);  // the last access: the waiter may go
        taken = next;
    }
}

/// FUTEX_REQUEUE as Linux's futex_requeue: wakes up to `wake_count` waiters of `from`, moves up to
/// `move_count` more to `to` in the order they came, and returns how many it woke and moved.
long futex_requeue(std::uint64_t from, std::uint64_t to, std::int32_t wake_count, std::int32_t move_count) {
    auto &source = bucket_of(from);
    auto &target = bucket_of(to);
    Waiter *taken = nullptr;                           // woken, off the list, linked through next
    Waiter *moving = nullptr, *moving_last = nullptr;  // off the list, in their order, linked through next
    long woken = 0, moved = 0;
    {
        const LockedPair locked(source, target);
        for (Waiter *waiter = source.first; waiter != nullptr && (woken < wake_count || moved < move_count);) {
            Waiter *const next = waiter->next;
            if (waiter->address == from) {
                remove(source, *waiter);
                if (woken < wake_count) {
                    waiter->woken.store(true, std::memory_order_release);
                    waiter->next = taken;
                    taken = waiter;
                    ++woken;
                } else {
                    waiter->next = nullptr;
                    (moving_last != nullptr ? moving_last->next : moving) = waiter;
                    moving_last = waiter;
                    ++moved;
                }
            }
            waiter = next;
        }
        // Appended after the walk, which so cannot meet them again when `to` shares the bucket.
        while (moving != nullptr) {
            Waiter *const next = moving->next;
            moving->address = to;
            append(target, *moving);
            moving->bucket.store(&target, std::memory_order_release);
            moving = next;
        }
    }
    release(taken);
    return woken + moved;
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
    release(taken);
    return woken;
}

long sys_futex(std::uint64_t address, int operation, std::uint32_t value, std::uint64_t timeout_address,
               std::uint64_t address2, std::uint32_t bitset) {
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
        case linux_futex::requeue: {
            // The count to move comes in the timeout's place. Both counts are ints, and the second word
            // must be aligned as well.
            const auto wake_count = static_cast<std::int32_t>(value);
            const auto move_count = static_cast<std::int32_t>(static_cast<std::uint32_t>(timeout_address));
            if (wake_count < 0 || move_count < 0 || address2 % 4 != 0) return -linux_errno::einval;
            return futex_requeue(address, address2, wake_count, move_count);
        }
        default: break;
    }
    // FUTEX_CMP_REQUEUE, wake-op, priority inheritance and bitsets other than "any" follow when a program
    // needs them.
    char message[80];
    const int length = std::snprintf(message, sizeof message, "apkrt: futex operation %#x is not supported yet\n",
                                     static_cast<unsigned>(operation));
    if (length > 0) static_cast<void>(::write(STDERR_FILENO, message, static_cast<std::size_t>(length)));
    return -linux_errno::enosys;
}

}  // namespace apkrt::sys
