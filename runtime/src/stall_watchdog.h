// Minato's stall diagnostic (not an Android feature): it watches each app surface -- a SurfaceView's surface or the
// native window handed to the app (apkrt_stall_surface_created). When a surface shows no first frame within a timeout,
// or a continuous renderer stops presenting after it had, the watchdog dumps every thread's stack once into the
// session log, so a black screen names where each thread waits. A present on another window (the app's decor) does not
// count as a surface's frame -- the Unity 6 case, where the decor drew while the SurfaceView never presented. After a
// return from the background, an app that keeps working with no frame (the reset race) is dumped once too. It
// reuses the ANR watchdog's SIGQUIT path for the managed (ART) threads and adds the native threads with symbols, the
// way process_exit's exit_callers line does. The decision (tick) is pure and tested; the daemon that runs it, the
// native dump and the signal are the host's side.
#pragma once

#include <cstdint>
#include <string>

namespace apkrt::stall {

/// The app resumed (APKRT_EVENT_RESUME): a paused app is watched again, and if a render surface presented earlier in
/// the session (a first load is the first-frame watch's), a return's watch starts at `now_ns`. The host replaces the
/// window and its surfaces at the background, so a return's surfaces are new: each live render surface with no frame
/// yet, and each one created while the watch runs, joins it with its return work then (set_return_work_source); a
/// surface that keeps its frame (the menu's pause) does not. The first present on any render surface after `now_ns`
/// ends the watch. When none comes within the return timeout while a joined surface's work grew -- the reset race, a
/// reload with no frame -- tick() dumps. Its own latch, once per session, apart from the first-frame/continuous one;
/// and no surface is dumped twice by either.
void resumed(std::uint64_t now_ns);

/// The app paused or stopped (APKRT_EVENT_PAUSE/STOP): report nothing until the next resume.
void paused();

/// An app surface was created at `now_ns` (apkrt_stall_surface_created from the host, when a SurfaceView's surface or a
/// NativeActivity's native window is handed to the app): the watchdog now expects a first frame on it within the
/// timeout. A second call for a live `id` is ignored.
void surface_created(std::uint64_t id, std::uint64_t now_ns);

/// A Java app's window was created at `now_ns` (apkrt_stall_window_created): its decor draws through hwui only when
/// something changed, and a WebView's page draws in the host's view, so the watchdog expects its first frame within the
/// timeout but never takes a still window, after any burst of frames, for a stall. A second call for a live `id` is
/// ignored.
void window_created(std::uint64_t id, std::uint64_t now_ns);

/// A frame was presented on the surface `id` at `now_ns` (apkrt_window_presented). A present on an `id` that is not a
/// tracked app surface -- the decor, a dialog -- is ignored: it is not that surface's frame.
void presented(std::uint64_t id, std::uint64_t now_ns);

/// Registers the source the watchdog pulls at each tick for a surface with no first frame yet (apkrt_stall_set_work
/// source). `source(id)` returns a cumulative, monotonically rising measure of real work done toward surface `id`'s
/// first frame -- the host sums it from live texture/buffer bytes resident (a high-water, so a render loop that reruns
/// the same uploads without a new result stays flat), bytes read from assets or the OBB, and shaders compiled. tick()
/// slides the no-first-frame deadline only when the pulled value exceeds the most this surface has reported, so the
/// B1 stall (uploads and draws that never present) does not slide it; a slow but advancing first load does. The source
/// returns 0 for an `id` it cannot measure (a Vulkan surface, a window with no live EGL context -- the decor), which
/// keeps the created-time deadline and the Unity 6 guard. It must be non-blocking and lock-free (an atomic read) and
/// must not call back into the watchdog -- tick() calls it while holding the watchdog lock. `nullptr` clears it (no
/// pull; every surface uses the created-time deadline). The hard cap (set_max_first_frame_ns) still reports a surface
/// that has gone too long with no first frame even while its work keeps growing.
void set_work_source(std::uint64_t (*source)(std::uint64_t id));

/// Installs the runtime's own default work source: a lock-free read of apkrt_stall_asset_work() for every surface, so
/// a surface whose first load reads assets or the OBB slides its no-first-frame deadline without any host or graphics
/// call (the Ren'Py 10-41 s first frame, which read a false stall before this). start_thread() calls it once. A
/// graphics source registered later (live texture/buffer bytes, per surface, apkrt_stall_set_work_source) supersedes
/// it. Process-wide, so it returns the same for every id; a surface advancing only on the GPU with no file reads still
/// relies on the hard cap.
void install_asset_work_source();

/// Registers the return watch's work source (apkrt_stall_set_return_work_source): a cumulative count of bytes moved
/// toward surface `id`, so a re-upload of the same scene grows it. Same rules as set_work_source. `nullptr` clears it:
/// the return watch then never dumps.
void set_return_work_source(std::uint64_t (*source)(std::uint64_t id));

/// Installs the runtime's own default return work source: apkrt_stall_asset_read_cumulative() for every surface (every
/// byte read from a regular file, re-reads counted). start_thread() calls it once; a graphics source registered later
/// supersedes it.
void install_asset_return_source();

/// The surface `id` was removed (surfaceDestroyed / apkrt_window_remove): stop watching it, so a surface torn down
/// before its first frame is not reported. An `id` that is not tracked is ignored.
void surface_removed(std::uint64_t id);

/// The watchdog's check at `now_ns`. Returns the reason to dump the first time a watched app surface has gone longer
/// than the timeout without its first frame (measured from the surface's creation), or a continuous renderer's surface
/// has gone longer than the timeout since its last frame, and latches, so it returns "" ever after: the dump is once
/// per session. Returns "" while paused, within the timeout, or once it has dumped. A return's watch (resumed()) has
/// its own once-per-session latch and returns its reason even after that dump, but never for a surface either watch
/// already reported. Pure: no log and no signal, so the host's daemon owns those.
std::string tick(std::uint64_t now_ns);

/// The no-present timeout; the default is 10 s. A test sets it small.
void set_timeout_ns(std::uint64_t ns);

/// The return watch's no-frame timeout, from the resume; the default is 15 s. A test sets it small.
void set_return_timeout_ns(std::uint64_t ns);

/// The hard cap on time to the first frame: a surface is reported this long after it was created even while its work
/// keeps growing, so a first load that advances forever or far too slowly cannot dodge the watchdog. Default 60 s; a
/// test sets it small.
void set_max_first_frame_ns(std::uint64_t ns);

/// Clears every bit of state (arm, present, latch, timeout) for a test. Not used in production.
void reset_for_test();

/// Starts the daemon that calls tick(clock) about once a second and, on a reason, writes the Minato line, dumps every
/// thread's stack (native with symbols) and raises SIGQUIT for the managed threads when one handles it. The host entry
/// starts it (activity-host / apk_run), not apkrt_session_begin, so a unit test that begins a session never runs it.
void start_thread();

/// Dumps every thread now with `reason`, exactly as the daemon does on a stall (each thread's tag, then its symbolized
/// stack, to stderr), but raises no signal. For a test to drive the native dump without the daemon's timing; the daemon
/// uses the same path in production.
void dump_threads_now(const std::string &reason);

}  // namespace apkrt::stall
