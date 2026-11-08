// Checks how the runtime ends the process for a guest program (process_exit.h): with the exit status
// Linux gives, after reporting it where apkrt_report_exit_status says, and what the run measured where
// apkrt_report_exit_measures says, for the app, which cannot wait for its own process. Each case ends a
// child process.
#include "process_exit.h"

#include "dl.h"
#include "syscall_frame.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <TargetConditionals.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
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

TEST(a_self_kill_names_its_cause_after_the_status) {
    // A guest that kills its own process leaves "without a status" (it reads as endedByIOS): note_self_kill records the
    // cause before the kill, and report_status appends it after the bare 128 + 9. The caller frames resolve against no
    // loaded guest image here, so each is a bare address -- the record's shape is what this checks (the privacy mask
    // and the guest names are checked by crash_report_test and the self-kill fixture).
    const auto ended = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt::process::note_self_kill(9, reinterpret_cast<std::uint64_t>(__builtin_frame_address(0)));
        apkrt::process::report_death_by(9);  // 128 + 9, as killed() reports a self-SIGKILL without a session
        apkrt::process::exit(0);             // ends the child; report_status already took the descriptor
    });
    CHECK(ended.report.starts_with("137\n"));
    CHECK(ended.report.find("killed itself with signal 9 (Killed)\n") != std::string::npos);
    CHECK(ended.report.find("\n#0 ") != std::string::npos);
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

TEST(the_last_16_failed_paths_are_named_newest_last) {
    // An app that ends itself with an error status after a file it needed was not there (a Python without its
    // stdlib): the paths the guest's lookups failed on, which no Android log names.
    apkrt::process::forget_failed_paths();
    CHECK(apkrt::process::failed_paths_lines().empty());
    for (int index = 0; index < 20; index++) {
        apkrt::process::note_failed_path("openat", "/data/lib/site" + std::to_string(index) + ".py", 2);
    }
    const std::string lines = apkrt::process::failed_paths_lines();
    CHECK(lines.find("site3.py") == std::string::npos);
    CHECK(lines.starts_with("W/apkrt   : failed path: openat /data/lib/site4.py ENOENT\n"));
    CHECK(lines.ends_with("W/apkrt   : failed path: openat /data/lib/site19.py ENOENT\n"));
    CHECK(std::count(lines.begin(), lines.end(), '\n') == 16);
}

TEST(only_a_missing_a_denied_or_a_wrong_path_counts_and_a_repeat_once) {
    apkrt::process::forget_failed_paths();
    apkrt::process::note_failed_path("openat", "/a", 2);
    apkrt::process::note_failed_path("openat", "/a", 2);  // the same lookup again in a row
    apkrt::process::note_failed_path("newfstatat", "/b", 13);
    apkrt::process::note_failed_path("faccessat", "/c", 20);
    apkrt::process::note_failed_path("openat", "/d", 17);  // EEXIST: the program asked for it
    CHECK(apkrt::process::failed_paths_lines() ==
          "W/apkrt   : failed path: openat /a ENOENT\n"
          "W/apkrt   : failed path: newfstatat /b EACCES\n"
          "W/apkrt   : failed path: faccessat /c ENOTDIR\n");
}

TEST(a_long_failed_path_is_cut_to_159_characters) {
    apkrt::process::forget_failed_paths();
    apkrt::process::note_failed_path("openat", "/" + std::string(400, 'x'), 2);
    const std::string lines = apkrt::process::failed_paths_lines();
    CHECK(lines == "W/apkrt   : failed path: openat /" + std::string(158, 'x') + " ENOENT\n");
}

TEST(a_status_other_than_0_in_an_app_process_logs_the_failed_paths_after_the_callers) {
    const auto ended = in_child([](int) {
        become_app_process();
        apkrt::process::forget_failed_paths();
        apkrt::process::note_failed_path("openat", "/data/lib/site.py", 2);
        apkrt::process::note_exit(1, 0);
        ::_exit(1);
    });
    const auto callers = ended.log.find("W/apkrt   : exit_group(1) from ");
    const auto path = ended.log.find("W/apkrt   : failed path: openat /data/lib/site.py ENOENT\n");
    CHECK(callers != std::string::npos && path != std::string::npos && callers < path);
}

TEST(the_output_tail_keeps_the_last_4_KiB_of_descriptors_1_and_2_as_lines) {
    apkrt::process::forget_output();
    CHECK(apkrt::process::output_lines().empty());
    const std::string traceback =
        "Traceback (most recent call last):\r\n  File \"site.py\", line 1\nImportError: no os\n";
    apkrt::process::note_output(2, traceback.data(), traceback.size());
    apkrt::process::note_output(1, "to stdout\n", 10);
    apkrt::process::note_output(3, "not kept\n", 9);  // another descriptor
    apkrt::process::note_output(2, "partial", 7);      // the last line without its newline
    CHECK(apkrt::process::output_lines() ==
          "W/apkrt   : stderr: Traceback (most recent call last):\n"
          "W/apkrt   : stderr:   File \"site.py\", line 1\n"
          "W/apkrt   : stderr: ImportError: no os\n"
          "W/apkrt   : stderr: to stdout\n"
          "W/apkrt   : stderr: partial\n");
}

TEST(the_output_tail_cuts_at_4_KiB_drops_the_cut_line_and_masks_control_bytes) {
    apkrt::process::forget_output();
    for (int index = 0; index < 600; index++) {
        const std::string line = "line " + std::to_string(index) + "\n";
        apkrt::process::note_output(2, line.data(), line.size());
    }
    apkrt::process::note_output(2, "a\x01\xffz\n", 5);
    const std::string lines = apkrt::process::output_lines();
    CHECK(lines.find("line 0\n") == std::string::npos);
    CHECK(lines.find("W/apkrt   : stderr: line 599\n") != std::string::npos);
    CHECK(lines.ends_with("W/apkrt   : stderr: a??z\n"));
    // The first kept line is whole: a line the cut split is dropped.
    CHECK(lines.starts_with("W/apkrt   : stderr: line "));
    const auto first_end = lines.find('\n');
    const std::string first = lines.substr(std::string("W/apkrt   : stderr: line ").size(),
                                           first_end - std::string("W/apkrt   : stderr: line ").size());
    CHECK(!first.empty() && std::stoi(first) > 0 && first.size() == std::to_string(std::stoi(first)).size());
    // One write larger than the ring keeps its end.
    apkrt::process::forget_output();
    const std::string big = std::string(10000, 'x') + "\nend\n";
    apkrt::process::note_output(1, big.data(), big.size());
    CHECK(apkrt::process::output_lines().ends_with("W/apkrt   : stderr: end\n"));
}

TEST(the_output_tail_leaves_out_the_log_lines_liblog_writes_to_descriptor_2) {
    // Minato's liblog writes "<priority>/<tag>: <message>" lines to stderr (runtime/bionic/src/liblog.cpp); they are
    // the runtime's, not the app's, and would push the app's own text out of the 4 KiB.
    apkrt::process::forget_output();
    const std::string log = "I/ActivityThread: starting\nW/apkrt   : some warning\n";
    apkrt::process::note_output(2, log.data(), log.size());
    apkrt::process::note_output(2, "ImportError: no os\n", 19);
    apkrt::process::note_output(2, "E/Tag: one", 10);  // a log line in two writes
    apkrt::process::note_output(2, " more\n", 6);
    apkrt::process::note_output(1, "F/x y: text\nF not a log line\n", 29);
    CHECK(apkrt::process::output_lines() ==
          "W/apkrt   : stderr: ImportError: no os\n"
          "W/apkrt   : stderr: F not a log line\n");
    // A flood of them does not evict the app's line.
    apkrt::process::forget_output();
    apkrt::process::note_output(2, "ImportError: no os\n", 19);
    for (int index = 0; index < 600; index++) {
        const std::string line = "D/Flood: line " + std::to_string(index) + "\n";
        apkrt::process::note_output(2, line.data(), line.size());
    }
    CHECK(apkrt::process::output_lines() == "W/apkrt   : stderr: ImportError: no os\n");
}

TEST(a_status_other_than_0_in_an_app_process_logs_the_output_after_the_failed_paths) {
    const auto ended = in_child([](int) {
        become_app_process();
        apkrt::process::forget_failed_paths();
        apkrt::process::forget_output();
        apkrt::process::note_failed_path("openat", "/data/lib/site.py", 2);
        apkrt::process::note_output(2, "ImportError: No module named site\n", 34);
        apkrt::process::note_exit(1, 0);
        ::_exit(1);
    });
    const auto path = ended.log.find("W/apkrt   : failed path: openat /data/lib/site.py ENOENT\n");
    const auto output = ended.log.find("W/apkrt   : stderr: ImportError: No module named site\n");
    CHECK(path != std::string::npos && output != std::string::npos && path < output);
}

// The syscall layer feeds both rings: a failed openat and what the guest writes to descriptors 1 and 2 (write and
// writev), whenever it happens, and the exit's log names them (Linux numbers: openat 56, write 64, writev 66).
long guest_call(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a0;
    frame.x[1] = a1;
    frame.x[2] = a2;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

TEST(the_syscall_layer_keeps_a_failed_path_and_the_output_for_the_exit_s_log) {
    const auto ended = in_child([](int) {
        become_app_process();
        apkrt::process::forget_failed_paths();
        apkrt::process::forget_output();
        const char *const missing = "/no/such/lib/python2.7/site.pyo";
        CHECK(guest_call(56, static_cast<std::uint64_t>(-100), reinterpret_cast<std::uint64_t>(missing)) < 0);
        // The guest's descriptors 1 and 2 are a pipe's write end here (Linux numbers: pipe2 59, dup3 24).
        int pipe[2] = {-1, -1};
        CHECK(guest_call(59, reinterpret_cast<std::uint64_t>(pipe), 0) == 0);
        CHECK(guest_call(24, static_cast<std::uint64_t>(pipe[1]), 2) == 2);
        CHECK(guest_call(24, static_cast<std::uint64_t>(pipe[1]), 1) == 1);
        const std::string one = "ImportError: No module named site\n";
        CHECK(guest_call(64, 2, reinterpret_cast<std::uint64_t>(one.data()), one.size()) ==
              static_cast<long>(one.size()));
        char first[] = "vector ", second[] = "part\n";
        iovec vectors[2] = {{first, 7}, {second, 5}};
        CHECK(guest_call(66, 1, reinterpret_cast<std::uint64_t>(vectors), 2) == 12);
        char other[] = "not kept\n";
        guest_call(64, 0, reinterpret_cast<std::uint64_t>(other), 9);  // descriptor 0: a read-only end, fails or not
        apkrt::process::note_exit(1, 0);
        ::_exit(1);
    });
    CHECK(ended.log.find("W/apkrt   : failed path: openat /no/such/lib/python2.7/site.pyo ENOENT\n") !=
          std::string::npos);
    CHECK(ended.log.find("W/apkrt   : stderr: ImportError: No module named site\n") != std::string::npos);
    CHECK(ended.log.find("W/apkrt   : stderr: vector part\n") != std::string::npos);
    CHECK(ended.log.find("stderr: not kept") == std::string::npos);
}

TEST(the_failed_paths_stay_kept_whatever_the_app_drew) {
    apkrt::process::forget_failed_paths();
    apkrt::process::note_failed_path("openat", "/before", 2);
    apkrt::process::note_failed_path("openat", "/after", 2);
    CHECK(apkrt::process::failed_paths_lines() ==
          "W/apkrt   : failed path: openat /before ENOENT\nW/apkrt   : failed path: openat /after ENOENT\n");
}

TEST(a_status_of_0_logs_no_failed_paths) {
    const auto ended = in_child([](int) {
        become_app_process();
        apkrt::process::forget_failed_paths();
        apkrt::process::forget_output();
        apkrt::process::note_output(2, "text\n", 5);
        apkrt::process::note_failed_path("openat", "/data/lib/site.py", 2);
        apkrt::process::note_exit(0, 0);
        ::_exit(0);
    });
    CHECK(ended.log.empty());
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

/// A drain that marks in the log where it ran.
void mark_drained() {
    static_cast<void>(::write(STDERR_FILENO, "drained\n", 8));
}

// The host's log drain (apkrt_drain_log_at_end, ADR-0070) runs after the end's log line, status and measures, once,
// for an exit and for a signal death, and with no session too.
TEST(the_log_drain_runs_once_after_the_end_line) {
    const auto exited = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt_drain_log_at_end(mark_drained);
        become_app_process();
        apkrt::process::exit(3);
    });
    CHECK(WIFEXITED(exited.status) && WEXITSTATUS(exited.status) == 3);
    CHECK(exited.log == "I/Zygote  : Process " + std::to_string(exited.pid) + " exited cleanly (3)\ndrained\n");
    const auto signaled = in_child([](int report) {
        apkrt_report_exit_status(report);
        apkrt_drain_log_at_end(mark_drained);
        become_app_process();
        apkrt::process::report_death_by(6);  // SIGABRT
        apkrt::process::exit(0);
    });
    CHECK(signaled.report == "134\n");
    CHECK(signaled.log == "I/Zygote  : Process " + std::to_string(signaled.pid) +
                              " exited due to signal 6 (Aborted)\ndrained\n");
    const auto plain = in_child([](int) {
        apkrt_drain_log_at_end(mark_drained);
        apkrt::process::exit(4);
    });
    CHECK(plain.log == "drained\n");
    const auto off = in_child([](int) {
        apkrt_drain_log_at_end(mark_drained);
        apkrt_drain_log_at_end(nullptr);
        become_app_process();
        apkrt::process::report_death_by(11);
        ::_exit(0);
    });
    CHECK(off.log.find("drained") == std::string::npos);
}

/// The app's stderr in a child: a pipe that a thread copies on to the stderr before it, slowly.
int stderr_before = -1;
std::atomic<bool> copied{false};

void restore_and_wait() {
    ::dup2(stderr_before, STDERR_FILENO);  // the pipe's last write end: the thread reads to its end
    for (int waited = 0; !copied.load() && waited < 1000; ++waited) {
        const timespec millisecond{0, 1'000'000};
        ::nanosleep(&millisecond, nullptr);
    }
}

// What a drain is for: a crash ends the process at once, and the lines a thread still has to copy out of a pipe
// would go with it.
TEST(a_drain_lets_a_piped_log_reach_its_end_before_a_crash) {
    const auto ended = in_child([](int) {
        int ends[2];
        CHECK(::pipe(ends) == 0);
        stderr_before = ::dup(STDERR_FILENO);
        ::dup2(ends[1], STDERR_FILENO);
        ::close(ends[1]);
        std::thread([reading = ends[0]] {
            char buffer[256];
            for (ssize_t count; (count = ::read(reading, buffer, sizeof buffer)) > 0;) {
                std::this_thread::sleep_for(20ms);
                static_cast<void>(::write(stderr_before, buffer, static_cast<std::size_t>(count)));
            }
            copied.store(true);
        }).detach();
        apkrt_drain_log_at_end(restore_and_wait);
        become_app_process();
        static_cast<void>(::write(STDERR_FILENO, "F/libc  : Fatal signal 11\n", 26));
        apkrt::process::report_death_by(11);
        ::_exit(139);  // as the raised signal ends it: at once
    });
    CHECK(ended.log == "F/libc  : Fatal signal 11\nI/Zygote  : Process " + std::to_string(ended.pid) +
                           " exited due to signal 11 (Segmentation fault)\n");
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
