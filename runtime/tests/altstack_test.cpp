// Checks the fatal guard for a guest thread's alternate signal stack (threads.cpp run()). The runtime's signal
// handlers use SA_ONSTACK, and the runtime returns into islands without a settle, so a handler must land on an
// alternate stack: a fault may have used up the guest thread's own stack. A thread that could not get one must not
// run, so when the install fails run() writes a clear message to stderr and ends the process instead of continuing
// silently. The test forces the install to fail through a test seam (threads::test_force_altstack_failure) and
// checks that a child process ends with the message, rather than running on.
#include "threads.h"

#include "check.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <string>

namespace {

struct Ended {
    int status = 0;   // as waitpid returns it
    std::string log;  // what the child wrote to stderr
};

/// Runs `body` in a child process whose stderr is captured, then waits for it (modelled on exit_test.cpp's
/// in_child). The body must end the process itself; a return is reported as exit status 99.
template <typename Body>
Ended in_child(Body body) {
    int logs[2];
    CHECK(::pipe(logs) == 0);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(logs[0]);
        ::dup2(logs[1], STDERR_FILENO);
        ::close(logs[1]);
        body();
        ::_exit(99);
    }
    ::close(logs[1]);
    Ended ended{};
    char buffer[128];
    for (ssize_t count; (count = ::read(logs[0], buffer, sizeof buffer)) > 0;) {
        ended.log.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(logs[0]);
    ::waitpid(child, &ended.status, 0);
    return ended;
}

// The guard ends the process loudly; without it the thread would run on with no alternate stack. The child first
// disables any alternate stack (AddressSanitizer installs one per thread, which would make run() skip the install),
// so run() tries to install its own, which the seam forces to fail.
TEST(a_guest_thread_that_cannot_get_an_alternate_signal_stack_ends_the_process) {
    const auto ended = in_child([] {
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        off.ss_size = 64 * 1024;  // Darwin wants a size even to disable it
        ::sigaltstack(&off, nullptr);
        apkrt::threads::test_force_altstack_failure = true;
        apkrt::threads::GuestThread thread;
        apkrt::threads::run(thread, [](void *) {}, nullptr);  // the guard ends the process before enter runs
    });
    CHECK(WIFEXITED(ended.status) && WEXITSTATUS(ended.status) == 127);
    CHECK(ended.log.find("alternate signal stack") != std::string::npos);
}

}  // namespace

int main() { return apk::testing::run_all(); }
