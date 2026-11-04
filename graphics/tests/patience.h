// A limit for graphics-bench-host's and graphics-app-test's waits on a program, counted in the waiter's own rounds:
// each round counts its real length, at most 10 ms more than the round it asked for. A busy Mac that starves the
// waiter (and the program with it) stretches the limit instead of spending it, so a wait ends early only for a
// program that hangs; a sleep or a stop of the whole test counts nothing.
#ifndef PATIENCE_H
#define PATIENCE_H

#include <algorithm>
#include <chrono>

class Patience {
public:
    explicit Patience(std::chrono::nanoseconds limit) : left_(limit) {}

    /// After a round that asked for `asked` (its sleep or timed wait): false once the rounds add up to the limit.
    bool more(std::chrono::nanoseconds asked) {
        const auto now = std::chrono::steady_clock::now();
        left_ -= std::min<std::chrono::nanoseconds>(now - last_, asked + std::chrono::milliseconds(10));
        last_ = now;
        return left_ > std::chrono::nanoseconds::zero();
    }

private:
    std::chrono::nanoseconds left_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

#endif
