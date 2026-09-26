// How a guest program ends the process (ADR-0019: one program per process). Every end goes through
// here, so that the runtime can report the exit status first to a descriptor the app chose
// (apkrt_report_exit_status): the app cannot wait for its own process, and devicectl's exit code
// is not reliable for a process that ends right after its start. With a session the program's end
// leaves the process to the session's end handler (ADR-0028, decision 5).
#pragma once

namespace apkrt::process {

/// The program's end through exit_group, with its last thread or by linker64's failure: reports
/// `status`; then, with a session, has every other guest thread stop, calls the end handler on the
/// calling thread and stops it; without one, ends the process with `status` as exit() does. An end
/// that comes after another one only stops the calling thread.
[[noreturn]] void end(int status);

/// Whether the program has ended with a session: a guest thread then stops (stop()) where it would go
/// back to guest code, instead of delivering signals.
bool ended();

/// Stops the calling thread for good: it blocks every signal, sleeps, and never runs guest code again.
[[noreturn]] void stop();

/// Where to report the exit status, as decimal text and a newline; -1 for nowhere.
void report_exit_status_to(int descriptor);

/// Reports 128 + the Linux signal `signal`, as a shell shows a process that the signal ends; for
/// the caller that ends it by the signal next. Safe in a signal handler: it only calls write().
void report_death_by(int signal);

/// Reports `status` and ends the process with it, keeping the low 8 bits as Linux does.
[[noreturn]] void exit(int status);

}  // namespace apkrt::process
