// Checks how the runtime ends the process for a guest program (process_exit.h): with the exit status
// Linux gives, after reporting it where apkrt_report_exit_status says, for the app, which cannot wait
// for its own process. Each case ends a child process.
#include "process_exit.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace {

struct Ended {
    int status;          // as waitpid returns it
    std::string report;  // what the child wrote to the report descriptor
};

/// Runs `body` in a child process, which gets the write end of a pipe and must end the process.
template <typename Body>
Ended in_child(Body body) {
    int ends[2];
    CHECK(::pipe(ends) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(ends[0]);
        body(ends[1]);
        ::_exit(99);
    }
    ::close(ends[1]);
    Ended ended{0, ""};
    char buffer[64];
    for (ssize_t count; (count = ::read(ends[0], buffer, sizeof buffer)) > 0;) {
        ended.report.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(ends[0]);
    ::waitpid(child, &ended.status, 0);
    return ended;
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

}  // namespace

int main() {
    return apk::testing::run_all();
}
