// How a guest program ends the process (ADR-0019: one program per process). Every end goes through
// here, so that the runtime can report the exit status first to a descriptor the app chose
// (apkrt_report_exit_status): the app cannot wait for its own process, and devicectl's exit code
// is not reliable for a process that ends right after its start. What the run measured follows,
// to a descriptor of its own (apkrt_report_exit_measures). With a session the program's end
// leaves the process to the session's end handler (ADR-0028, decision 5).
#pragma once

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

/// Reports `status` and the measures and ends the process with the status, keeping the low 8 bits as
/// Linux does.
[[noreturn]] void exit(int status);

}  // namespace apkrt::process
