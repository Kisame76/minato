// Minato's stall diagnostic (not an Android feature): when a resumed app shows no first frame within a timeout, or
// stops presenting after it had, the watchdog dumps every thread's stack once into the session log, so a black screen
// names where each thread waits. It reuses the ANR watchdog's SIGQUIT path for the managed (ART) threads and adds the
// native threads with symbols, the way process_exit's exit_callers line does. The decision (tick) is pure and tested;
// the daemon that runs it, the native dump and the signal are the host's side.
#pragma once

#include <cstdint>
#include <string>

namespace apkrt::stall {

/// The app resumed at `now_ns` (APKRT_EVENT_RESUME): arm the timer. A later resume keeps the one-per-session latch.
void resumed(std::uint64_t now_ns);

/// The app paused or stopped (APKRT_EVENT_PAUSE/STOP): disarm until the next resume.
void paused();

/// A frame was presented at `now_ns` (apkrt_window_presented): the latest activity the timer measures from.
void presented(std::uint64_t now_ns);

/// The watchdog's check at `now_ns`. Returns the reason to dump the first time the app has been resumed and has gone
/// longer than the timeout without a present -- measured from the last present, or from the resume before the first
/// frame -- and latches, so it returns "" ever after: the dump is once per session. Returns "" while paused, within
/// the timeout, or once it has dumped. Pure: no log and no signal, so the host's daemon owns those.
std::string tick(std::uint64_t now_ns);

/// The no-present timeout; the default is 10 s (the lead's figure). A test sets it small.
void set_timeout_ns(std::uint64_t ns);

/// Clears every bit of state (arm, present, latch, timeout) for a test. Not used in production.
void reset_for_test();

/// Starts the daemon that calls tick(clock) about once a second and, on a reason, writes the Minato line, dumps every
/// thread's stack (native with symbols) and raises SIGQUIT for the managed threads when one handles it. The host entry
/// starts it (activity-host / apk_run), not apkrt_session_begin, so a unit test that begins a session never runs it.
void start_thread();

}  // namespace apkrt::stall
