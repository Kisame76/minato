// The stall/ANR thread dump tags each thread with its name (the guest and the framework name threads with
// prctl(PR_SET_NAME), which the syscall layer maps to the host's pthread_setname_np; Minato names its own helper
// threads the same way), so a return-bug report names the thread that stopped answering -- the render thread, the
// periodic audio thread -- not just an index. This drives the native dump directly (dump_threads_now) with a parked
// thread of a known name and asserts the name shows in the dumped line. The dump runs in a child whose stderr is a
// pipe, so the parent reads the whole dump off the pipe without touching the test runner's own stderr, and a
// terminate in the dump path stays contained -- the same isolation the watchdog's own test uses.
#include "stall_watchdog.h"

#include "check.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::atomic<bool> probe_named{false};

// A thread that names itself, as a guest render/audio thread does, then parks with a still stack (a hung thread): the
// case the dump is for. The child _exit ends it, so it is never joined.
void *name_and_park(void *) {
    ::pthread_setname_np("RenderProbe");
    probe_named.store(true, std::memory_order_release);
    for (;;) ::usleep(1'000);
    return nullptr;
}

[[noreturn]] void dump_in_child(int writer) {
    pthread_t probe{};
    ::pthread_create(&probe, nullptr, name_and_park, nullptr);
    while (!probe_named.load(std::memory_order_acquire)) ::usleep(1'000);  // the name is set before the dump reads it
    ::dup2(writer, STDERR_FILENO);  // the native dump writes to STDERR_FILENO
    apkrt::stall::dump_threads_now("test stall");
    ::_exit(0);
}

// Runs the dump in a forked child and returns everything it wrote to stderr. The parent is single-threaded here, so
// the fork is clean; it reads the pipe while the child writes, so a dump larger than the pipe buffer cannot wedge it.
std::string capture_dump() {
    int fds[2];
    if (::pipe(fds) != 0) return {};
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        dump_in_child(fds[1]);
    }
    ::close(fds[1]);
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fds[0], buffer, sizeof buffer);
        if (got <= 0) break;
        out.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return out;
}

}  // namespace

TEST(the_thread_dump_tags_each_thread_with_its_name) {
    const std::string dump = capture_dump();
    // The dump still opens with its reason and marks each thread.
    CHECK(dump.find("test stall") != std::string::npos);
    CHECK(dump.find("thread ") != std::string::npos);
    // ...and it carries the parked thread's name, so the hung thread is named, not just numbered. The render thread
    // and the periodic audio thread name themselves the same way and show up the same.
    CHECK(dump.find("RenderProbe") != std::string::npos);
}

TEST(the_thread_dump_puts_minatos_own_threads_under_a_line_of_their_own) {
    // Here no thread runs guest code, so every thread is Minato's own: each comes after the line that says so, and
    // none before it. In a session the app's threads come first.
    const std::string dump = capture_dump();
    const std::size_t line = dump.find("Minato's own threads, not the app's:");
    CHECK(line != std::string::npos);
    CHECK(dump.find("thread ") > line);
    CHECK(dump.find("RenderProbe") > line);
}

int main() { return apk::testing::run_all(); }
