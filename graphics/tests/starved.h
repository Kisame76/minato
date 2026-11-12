// Which of the process's own threads the machine starved, for graphics-app-test's live frame intervals: a thread
// that the kernel holds runnable (TH_STATE_RUNNING: on a CPU or waiting in the run queue) while its CPU time stands
// still was ready to work and got no CPU, so the time went to other processes. A thread that waits (for a fence, a
// tick, a lock, a sleep) is not runnable: a wait of our own never counts, so a real pacing bug still fails the
// interval rule on a loaded Mac. graphics-app-test's canary thread only saw its own gaps, and a frame producer
// starved while the canary ran was taken for a pacing bug (graphics.vulkan_live under load 28-79: an interval of
// 250 ms, the canary's longest gap 0 ms).
//
// StarvedRuns classifies samples; ThreadSampler takes them from the task's threads every few milliseconds.
#ifndef STARVED_H
#define STARVED_H

#include <mach/mach.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

/// One look at a thread (its 64-bit id, THREAD_IDENTIFIER_INFO's, as port names do not last): when (ns,
/// CLOCK_UPTIME_RAW), its CPU time so far (user and system, us) and whether the kernel held it runnable.
struct ThreadSample {
    std::uint64_t thread;
    std::uint64_t at_ns;
    std::uint64_t cpu_us;
    bool runnable;
};

/// The starved runs of each thread: between two looks at the same thread that both found it runnable, the thread
/// was starved when its CPU time grew by less than a tenth of the time between them. Consecutive starved spans of
/// a thread join into a run; runs of at least `shortest_ns` are kept.
class StarvedRuns {
public:
    explicit StarvedRuns(std::uint64_t shortest_ns = 30'000'000) : shortest_ns_(shortest_ns) {}

    void add(const ThreadSample &sample) {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto found = threads_.find(sample.thread);
        if (found == threads_.end()) {
            threads_.emplace(sample.thread, State{sample, 0, false});
            return;
        }
        State &state = found->second;
        const ThreadSample &last = state.last;
        const std::uint64_t wall = sample.at_ns > last.at_ns ? sample.at_ns - last.at_ns : 0;
        const std::uint64_t cpu_ns = (sample.cpu_us > last.cpu_us ? sample.cpu_us - last.cpu_us : 0) * 1000;
        const bool starved = last.runnable && sample.runnable && wall > 0 && cpu_ns * 10 < wall;
        if (starved && !state.open) {
            state.open = true;
            state.since = last.at_ns;
        } else if (!starved && state.open) {
            close(state, last.at_ns);
        }
        state.last = sample;
    }

    /// Ends every open run at the threads' last look (call once the sampling stopped).
    void finish() {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto &entry : threads_) {
            if (entry.second.open) close(entry.second, entry.second.last.at_ns);
        }
    }

    /// The longest kept run that overlaps [start, end), in ms; 0 for none.
    double longest(std::uint64_t start, std::uint64_t end) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::uint64_t longest = 0;
        for (const auto &run : runs_) {
            if (run.first < end && run.second > start) longest = std::max(longest, run.second - run.first);
        }
        return static_cast<double>(longest) / 1e6;
    }

private:
    struct State {
        ThreadSample last;
        std::uint64_t since;
        bool open;
    };

    void close(State &state, std::uint64_t until) {
        state.open = false;
        if (until - state.since >= shortest_ns_) runs_.emplace_back(state.since, until);
    }

    const std::uint64_t shortest_ns_;
    mutable std::mutex mutex_;
    std::map<std::uint64_t, State> threads_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> runs_;  // [start, end), ns
};

/// Looks at every thread of the task but its own every `period_ms` and gives the samples to `runs`.
class ThreadSampler {
public:
    explicit ThreadSampler(StarvedRuns &runs, unsigned period_ms = 5) : runs_(runs), period_ms_(period_ms) {}
    ThreadSampler(const ThreadSampler &) = delete;
    ThreadSampler &operator=(const ThreadSampler &) = delete;

    void start() {
        running_ = true;
        thread_ = std::thread([this] {
            const thread_act_t self = mach_thread_self();
            while (running_) {
                look(self);
                std::this_thread::sleep_for(std::chrono::milliseconds(period_ms_));
            }
            mach_port_deallocate(mach_task_self(), self);
        });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        runs_.finish();
    }

    ~ThreadSampler() { stop(); }

private:
    void look(thread_act_t self) {
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
        const std::uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            thread_basic_info_data_t info;
            thread_identifier_info_data_t id;
            mach_msg_type_number_t size = THREAD_BASIC_INFO_COUNT;
            mach_msg_type_number_t id_size = THREAD_IDENTIFIER_INFO_COUNT;
            if (threads[i] != self &&
                thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &size) ==
                    KERN_SUCCESS &&
                thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&id), &id_size) ==
                    KERN_SUCCESS) {
                const auto seconds = static_cast<std::uint64_t>(info.user_time.seconds + info.system_time.seconds);
                const auto micros =
                    static_cast<std::uint64_t>(info.user_time.microseconds + info.system_time.microseconds);
                runs_.add({id.thread_id, now, seconds * 1'000'000 + micros, info.run_state == TH_STATE_RUNNING});
            }
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    }

    StarvedRuns &runs_;
    const unsigned period_ms_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

#endif
