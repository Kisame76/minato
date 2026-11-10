// How a guest program ends the process (ADR-0019: one program per process). Every end goes through
// here, so that the runtime can report the exit status first to a descriptor the app chose
// (apkrt_report_exit_status): the app cannot wait for its own process, and devicectl's exit code
// is not reliable for a process that ends right after its start. What the run measured follows,
// to a descriptor of its own (apkrt_report_exit_measures). With a session the program's end
// leaves the process to the session's end handler (ADR-0028, decision 5).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace apkrt::process {

/// The program's end through exit_group, with its last thread or by linker64's failure: reports
/// `status` and the measures; then, with a session, has every other guest thread stop, calls the
/// end handler on the calling thread and stops it; without one, ends the process with `status` as
/// exit() does. An end that comes after another one only stops the calling thread.
[[noreturn]] void end(int status);

/// Whether the program has ended with a session: a guest thread then stops (stop()) where it would go
/// back to guest code, instead of delivering signals.
bool ended();

/// Stops the calling thread for good: it blocks every signal, sleeps, and never runs guest code again.
[[noreturn]] void stop();

/// Where to report the exit status, as decimal text and a newline; -1 for nowhere.
void report_exit_status_to(int descriptor);

/// Where to report the measures after the status, as apkrt_report_exit_measures says; -1 for nowhere.
void report_exit_measures_to(int descriptor);

/// The program starts (apkrt_start_program): takes what the measures need from its start, the
/// clock, the process's start time and on iOS the memory available, since a signal handler at the
/// end could not take them. Without it the end reports no measures.
void program_starts();

/// Reports 128 + the Linux signal `signal`, as a shell shows a process that the signal ends, and the
/// measures; for the caller that ends it by the signal next. Safe in a signal handler: it only calls
/// write(), the clock and task_info.
void report_death_by(int signal);

/// SIGKILL to the guest's own process, which nothing catches, blocks or ignores (ADR-0028, changed 2026-10-07):
/// Linux ends every thread at once, so the calling thread ends the program right in its syscall. With a session as
/// end() does, with 128 + 9 as a shell shows the kill, so that the app goes back to its library; without one by the
/// host's SIGKILL after the report, as when a signal's default action ends the process.
[[noreturn]] void killed();

/// A crash names its cause (ADR-0032 decision 8): when a guest kills its own process -- kill/tgkill/tkill of itself
/// with `signal` (always SIGKILL, the only self-signal the runtime ends the process on; Process.killProcess and a
/// Crashlytics NDK re-raise reach it too) -- records, before the kill, a bounded async-signal-safe line
/// "killed itself with signal <N> (<name>)\n" and up to 8 frames of the caller's guest frame-pointer chain from
/// `caller_fp` (x29 at the syscall), each as "#<n> <library>+0x<offset>\n". report_status appends the record to the
/// last-session status after the bare exit number, so an app whose process ended "without a status" still shows the
/// cause. Privacy (crash_report.h): only an engine/Android/Minato library appears by name, any other as "app library
/// N"; an address in no loaded guest image (a rewriter stub lies below every image) is a bare "0x<address>", never a
/// foreign .so offset. Called in the syscall's normal context, so it may allocate for the lookup; the later append is
/// only a write(). A second call (another thread racing to self-kill) is ignored: the first record stands.
void note_self_kill(int signal, std::uint64_t caller_fp);

/// A crash names its cause (ADR-0032 decision 8), the fatal-signal half: at the terminal point of a guest fatal signal
/// -- after ART's fault manager and every guest handler have declined (signals.cpp, right before die_by) -- records a
/// bounded, ASYNC-SIGNAL-SAFE record: "signal: <N> (<name>)", "code: <C>", "fault: 0x<addr>", "pc: <frame>", "lr:
/// <frame>", then up to 8 "#<n> <frame>" of the guest frame-pointer chain from `frame_pointer`. A
/// frame is "<library>+0x<offset>" (guest vaddr) for a listed engine/Android/Minato library, "app library N+..." for
/// any other (privacy), "0x<addr>" for an address in no module (crash_report.h). `code` is the Linux si_code and
/// `fault_address` the faulting address (both 0 for a non-fault terminating signal through deliver_pending). Uses only
/// the lock-free module registry (register_module) and a fault-safe backtrace, so no allocation and no lock the
/// faulting thread may hold; report_status appends it after the bare 128 + signal number. The first cause recorded
/// wins (it shares the self-kill record's slot: a process dies of one cause).
void note_signal(int signal, int code, std::uint64_t fault_address, std::uint64_t pc, std::uint64_t lr,
                 std::uint64_t frame_pointer);

/// For an app's log, the guest's callers of exit_group with a `status` other than 0, which no Android log names, so
/// that an app that ends itself with an error status shows where (a Python SystemExit is Py_Exit's exit): one line,
/// "W/apkrt   : exit_group(<status>) from <symbol>+<offset> (<image>) <- ...", up to `limit` frames up the guest's
/// frame-pointer chain from `frame_pointer` (x29); empty for status 0.
std::string exit_callers(int status, std::uint64_t frame_pointer, int limit = 8);

/// The last 16 paths the guest's lookups failed on (a missing file, a denied one, a component that is no directory),
/// for the log of an app that ends itself with an error status after a file it needed was not there, whose own
/// message nobody reads (a Python without its stdlib exits with 1 and an error on a pipe). `syscall` names the call
/// ("openat"), `path` is the guest's path, `error` the Linux errno; other errors are not kept, and the same call on the
/// same path again in a row is kept once. A path is cut to 159 characters. Safe from any thread.
void note_failed_path(const char *syscall, const std::string &path, int error);

/// "W/apkrt   : failed path: <syscall> <path> <ENOENT|EACCES|ENOTDIR>\n" for each kept path, oldest first; empty
/// for none.
std::string failed_paths_lines();

/// Forgets the kept paths, for tests.
void forget_failed_paths();

/// What the guest writes to its descriptor 1 or 2 (write, writev), the last 4 KiB of it, for the log of an app that
/// ends itself with an error status: Android sends both to /dev/null for an app, so a Python's traceback or a runtime's
/// last words are lost there, and here they are only logged at the exit, never changed on their way. `fd` other than 1
/// or 2 is ignored. Safe from any thread.
void note_output(int fd, const char *data, std::size_t length);

/// "W/apkrt   : stderr: <line>\n" for each non-empty line of the kept output, oldest first (a first line the 4 KiB cut
/// is dropped; non-printable bytes show as '?', a carriage return is dropped); empty for none.
std::string output_lines();

/// Forgets the kept output, for tests.
void forget_output();

/// exit_group's note before end(): in an app process (one with a session, as the Zygote line), exit_callers' line to
/// stderr for a status other than 0, and the failed paths' and the kept output's lines after it.
void note_exit(int status, std::uint64_t frame_pointer);

/// Reports `status` and the measures and ends the process with the status, keeping the low 8 bits as
/// Linux does.
[[noreturn]] void exit(int status);

}  // namespace apkrt::process
