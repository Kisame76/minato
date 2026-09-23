// How a guest program ends the process (ADR-0019: one program per process). Every end goes through
// here, so that the runtime can report the exit status first to a descriptor the app chose
// (apkrt_report_exit_status): the app cannot wait for its own process, and devicectl's exit code
// is not reliable for a process that ends right after its start.
#pragma once

namespace apkrt::process {

/// Where to report the exit status, as decimal text and a newline; -1 for nowhere.
void report_exit_status_to(int descriptor);

/// Reports 128 + the Linux signal `signal`, as a shell shows a process that the signal ends; for
/// the caller that ends it by the signal next. Safe in a signal handler: it only calls write().
void report_death_by(int signal);

/// Reports `status` and ends the process with it, keeping the low 8 bits as Linux does.
[[noreturn]] void exit(int status);

}  // namespace apkrt::process
