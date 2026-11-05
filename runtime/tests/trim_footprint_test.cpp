// The memory-footprint log at a trim (ADR-0035 item 3): when the host posts APKRT_EVENT_TRIM_MEMORY -- iOS warning the
// app of memory pressure, the kind Ren'Py hit -- the runtime writes one line to the session log with its phys_footprint
// and, on iOS, the memory it may still use. The line carries the logcat tag "apkrt", like the stall watchdog's lines,
// so it reaches the device's session log but java_app_run's app-tag comparison passes over it. The line is written
// only when MINATO_LOADER_LOG is set, as the loader's scudo line is (dl.cpp), so its machine-dependent MiB figures
// stay out of the byte-compared fixtures that post a trim (tools/run-fixture.cmake's window-trim-test); this test sets
// the env to see it. This begins a session, captures stderr around a running-critical trim (level 15, below
// UI_HIDDEN 20, so no window buffers are touched -- the footprint line alone), and checks the line's shape; the
// figures themselves are machine-dependent, so only the fixed text is asserted. Red before the log was added: a trim
// wrote nothing to stderr.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "check.h"

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

void on_end(void *, int) {}

TEST(a_trim_logs_the_memory_footprint_to_the_session_log) {
    apkrt_session session{};
    session.end = on_end;
    CHECK(apkrt_session_begin(&session) == 0);

    int ends[2];
    CHECK(::pipe(ends) == 0);
    const int saved = ::dup(STDERR_FILENO);
    CHECK(saved >= 0);
    // The log is gated on MINATO_LOADER_LOG (session.cpp), off in byte-compared fixtures; set it here to see the line.
    CHECK(::setenv("MINATO_LOADER_LOG", "1", 1) == 0);
    CHECK(::dup2(ends[1], STDERR_FILENO) >= 0);
    // No CHECK while stderr is the pipe: a failure message would go to the pipe, not the test's output.
    const std::uint32_t serial = apkrt_session_post(APKRT_EVENT_TRIM_MEMORY, 15, 0, 0);
    CHECK(::dup2(saved, STDERR_FILENO) >= 0);
    ::close(saved);
    ::close(ends[1]);

    CHECK(serial != 0);
    char buffer[512];
    const ssize_t got = ::read(ends[0], buffer, sizeof buffer - 1);
    ::close(ends[0]);
    CHECK(got > 0);
    const std::string line(buffer, static_cast<std::size_t>(got > 0 ? got : 0));
    CHECK(line.find("I/apkrt   : trim 15: footprint ") != std::string::npos);
    CHECK(line.find(" MiB, available ") != std::string::npos);
}

}  // namespace

int main() { return apk::testing::run_all(); }
