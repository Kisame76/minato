// Checks how the runtime ends the process for a guest program (process_exit.h): with the exit status
// Linux gives, after reporting it where apkrt_report_exit_status says, and what the run measured where
// apkrt_report_exit_measures says, for the app, which cannot wait for its own process. Each case ends a
// child process.
#include "process_exit.h"

#include "dl.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <TargetConditionals.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <thread>

// Two functions of this test whose names the end's line must give: the address past each one's only instruction stands
// for a guest's return address after a call that never returns, which lies at the next function's start.
extern "C" __attribute__((visibility("default"), noinline)) void exit_test_caller_a() { asm volatile(""); }
extern "C" __attribute__((visibility("default"), noinline)) void exit_test_caller_b() { asm volatile(""); }

namespace {

using namespace std::chrono_literals;

struct Ended {
    int status = 0;      // as waitpid returns it
    std::string report;  // what the child wrote to the report descriptor
    std::string log;     // what the child wrote to stderr (liblog's logcat -v tag sink)
    pid_t pid = 0;       // the child's pid, which the end's log line names
};

/// Runs `body` in a child process, which gets the write end of a pipe and must end the process. The
/// child's stderr is captured too, so a test can read the end's log line.
template <typename Body>
Ended in_child(Body body) {
    int ends[2], logs[2];
    CHECK(::pipe(ends) == 0);
    CHECK(::pipe(logs) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(ends[0]);
        ::close(logs[0]);
        ::dup2(logs[1], STDERR_FILENO);  // the end's log line goes to stderr, as liblog's sink does
        ::close(logs[1]);
        body(ends[1]);
        ::_exit(99);
    }
    ::close(ends[1]);
    ::close(logs[1]);
    Ended ended{};
    ended.pid = child;
    char buffer[64];
    for (ssize_t count; (count = ::read(ends[0], buffer, sizeof buffer)) > 0;) {
        ended.report.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(ends[0]);
    for (ssize_t count; (count = ::read(logs[0], buffer, sizeof buffer)) > 0;) {
        ended.log.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(logs[0]);
    ::waitpid(child, &ended.status, 0);
    return ended;
}

/// The measures' lines in their order and form (apkrt_report_exit_measures).
const std::regex measures_form("run-ms [0-9]+\\.[0-9]\n"
                               "process-ms [0-9]+\\.[0-9]\n"
                               "footprint-peak [0-9]+\n"
                               "resident-peak [0-9]+\n"
                               "virtual [0-9]+\n"
#if TARGET_OS_IPHONE
                               "available-at-start [0-9]+\n"
#endif
);

/// What follows `prefix` in `text`; empty when `text` does not start with it.
std::string after(const std::string &text, std::string_view prefix) {
    return text.starts_with(prefix) ? text.substr(prefix.size()) : std::string();
}

/// The value of the measure `name` in `measures`, which match measures_form; -1 without it.
double measure(const std::string &measures, const std::string &name) {
    const auto at = ("\n" + measures).find("\n" + name + " ");
    return at == std::string::npos ? -1 : std::stod(measures.substr(at + name.size() + 1));
}

/// A trivial end handler. These tests never run it (they call process::exit / report_death_by directly); it
/// only lets a session begin, so the child counts as an app process -- the one the end log line is for.
void app_end_handler(void *, int) {}

/// Begins a minimal session in the current (child) process, making it an app process.
void become_app_process() {
    const apkrt_session session{.context = nullptr, .end = app_end_handler};
    apkrt_session_begin(&session);
}

TEST(exit_ends_the_process_with_its_status_and_reports_it_first) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt::process::exit(42);
    });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 42);
    CHECK(ended.report == "42\n");
}

TEST(exit_keeps_the_low_8_bits_of_the_status_as_linux_does) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt::process::exit(0x1ff);
    });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 255);
    CHECK(ended.report == "255\n");
}

TEST(a_signal_that_ends_the_process_is_reported_as_128_plus_its_number_once) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt::process::report_death_by(11);  // SIGSEGV
        apkrt::process::exit(0);              // no second report
    });
    CHECK(ended.report == "139\n");
}

TEST(without_a_report_descriptor_the_process_just_ends) {
    const auto ended = in_child([](int) { apkrt::process::exit(7); });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 7);
    CHECK(ended.report.empty());
}

TEST(minus_one_turns_the_report_off) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt_report_exit_status(-1);
        apkrt::process::exit(3);
    });
    CHECK(WEXITSTATUS(ended.status) == 3);
    CHECK(ended.report.empty());
}

// The measures follow the status, in their order and form. run-ms counts at least the 50 ms from the program's
// start to its exit, and far less than 10 s, which microseconds would pass; process-ms that and the moment
// before the start. The memory is in use, and its resident peak lies inside the virtual size.
TEST(the_measures_follow_the_exit_status) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt_report_exit_measures(report);
        apkrt::process::program_starts();
        std::this_thread::sleep_for(50ms);
        apkrt::process::exit(42);
    });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 42);
    const auto measures = after(ended.report, "42\n");
    CHECK(std::regex_match(measures, measures_form));
    const double run = measure(measures, "run-ms"), process = measure(measures, "process-ms");
    CHECK(run >= 50 && run < 10'000);
    CHECK(process >= run && process < run + 10'000);
    CHECK(measure(measures, "footprint-peak") > 0);
    CHECK(measure(measures, "resident-peak") > 0);
    CHECK(measure(measures, "virtual") >= measure(measures, "resident-peak"));
}

TEST(a_signal_that_ends_the_process_reports_the_measures_once) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt_report_exit_measures(report);
        apkrt::process::program_starts();
        apkrt::process::report_death_by(11);  // SIGSEGV
        apkrt::process::exit(0);              // no second report
    });
    CHECK(std::regex_match(after(ended.report, "139\n"), measures_form));
}

// The measures need no report descriptor for the status: apk-run --measures sets none.
TEST(the_measures_go_out_without_the_exit_status_as_well) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_measures(report);
        apkrt::process::program_starts();
        apkrt::process::exit(0);
    });
    CHECK(std::regex_match(ended.report, measures_form));
}

TEST(without_a_measures_descriptor_only_the_status_goes_out) {
    const auto never = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt::process::program_starts();
        apkrt::process::exit(5);
    });
    CHECK(never.report == "5\n");
    const auto off = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt_report_exit_measures(report);
        apkrt_report_exit_measures(-1);
        apkrt::process::program_starts();
        apkrt::process::exit(5);
    });
    CHECK(off.report == "5\n");
}

// Without the program's start there is no run to measure.
TEST(without_a_program_start_no_measures_go_out) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_measures(report);
        apkrt::process::exit(6);
    });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 6);
    CHECK(ended.report.empty());
}

// The end goes to the log too, in `logcat -v tag` form (liblog.cpp), with the line Android's Zygote writes
// for a child that ended (com_android_internal_os_Zygote.cpp): "exited cleanly (<status>)" for an exit.
TEST(a_clean_exit_is_logged_as_zygote_does) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        become_app_process();
        apkrt::process::exit(42);
    });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 42);
    CHECK(ended.log == "I/Zygote  : Process " + std::to_string(ended.pid) + " exited cleanly (42)\n");
}

// A signal death gets Zygote's other line, with bionic's name for the signal (libc/private/bionic_sigdefs.h),
// and is logged once: the exit(0) that follows adds no second line.
TEST(a_signal_death_is_logged_with_its_bionic_name_once) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        become_app_process();
        apkrt::process::report_death_by(11);  // SIGSEGV
        apkrt::process::exit(0);              // no second report, no second log line
    });
    CHECK(ended.report == "139\n");
    CHECK(ended.log ==
          "I/Zygote  : Process " + std::to_string(ended.pid) + " exited due to signal 11 (Segmentation fault)\n");
}

// A plain program -- our NDK fixtures, run by apk-run on the Mac and FixtureRunner on the iPhone, which set a
// report descriptor but begin no session -- gets NO end line, as a shell-run program gets none from Zygote.
// Its status still goes to the report descriptor.
TEST(a_plain_program_without_a_session_logs_nothing) {
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt::process::exit(42);
    });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 42);
    CHECK(ended.report == "42\n");
    CHECK(ended.log.empty());
}

TEST(a_status_other_than_0_names_the_callers_up_the_frame_chain) {
    // A guest's frame records, as AArch64's frame-pointer chain has them: {the caller's frame, the return address}.
    const auto a = reinterpret_cast<std::uint64_t>(&exit_test_caller_a);
    const auto b = reinterpret_cast<std::uint64_t>(&exit_test_caller_b);
    std::uint64_t chain[6] = {0, a + 4, 0, b + 4, 0, 0};
    chain[0] = reinterpret_cast<std::uint64_t>(&chain[2]);
    chain[2] = reinterpret_cast<std::uint64_t>(&chain[4]);
    const std::string line = apkrt::process::exit_callers(1, reinterpret_cast<std::uint64_t>(&chain[0]));
    CHECK(line.starts_with("W/apkrt   : exit_group(1) from exit_test_caller_a+4 ("));
    CHECK(line.find(" <- exit_test_caller_b+4 (") != std::string::npos);
    CHECK(line.ends_with("\n"));
    CHECK(apkrt::process::exit_callers(0, reinterpret_cast<std::uint64_t>(&chain[0])).empty());
}

TEST(a_caller_frame_on_an_unreadable_page_ends_the_line_without_a_fault) {
    // A frame record whose caller lies above it and within 1 MiB, but on a page the process cannot read (a guard
    // page, a small thread's stack end): the line names the first caller and stops there.
    const auto page = static_cast<std::size_t>(::getpagesize());
    auto *memory = static_cast<std::uint64_t *>(
        ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    CHECK(memory != MAP_FAILED);
    CHECK(::mprotect(reinterpret_cast<char *>(memory) + page, page, PROT_NONE) == 0);
    memory[0] = reinterpret_cast<std::uint64_t>(memory) + page;  // the caller's frame: the unreadable page
    memory[1] = reinterpret_cast<std::uint64_t>(&exit_test_caller_a) + 4;
    const std::string line = apkrt::process::exit_callers(1, reinterpret_cast<std::uint64_t>(memory));
    CHECK(line.starts_with("W/apkrt   : exit_group(1) from exit_test_caller_a+4 ("));
    CHECK(line.find(" <- ") == std::string::npos);
    ::munmap(memory, 2 * page);
}

TEST(a_loader_held_by_another_thread_gives_addresses_not_a_wait) {
    // A guest thread inside the loader holds its lock while another one exits: the exit's line comes at once, with
    // what the loader would name as bare addresses.
    std::promise<void> held;
    std::promise<void> done;
    std::thread holder([&held, finished = done.get_future()]() mutable {
        const std::lock_guard lock(apkrt::dl::lock());
        held.set_value();
        finished.wait();
    });
    held.get_future().wait();
    std::uint64_t chain[2] = {0, reinterpret_cast<std::uint64_t>(&exit_test_caller_a) + 4};
    auto line = std::async(std::launch::async, [&chain] {
        return apkrt::process::exit_callers(1, reinterpret_cast<std::uint64_t>(&chain[0]));
    });
    const bool came = line.wait_for(2s) == std::future_status::ready;
    done.set_value();
    holder.join();
    CHECK(came);
    CHECK(line.get().starts_with("W/apkrt   : exit_group(1) from "));
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
