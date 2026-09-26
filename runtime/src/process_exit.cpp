#include "process_exit.h"

#include "APKRuntime/apkrt.h"
#include "session.h"
#include "signals.h"
#include "threads.h"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>

namespace apkrt::process {
namespace {

std::atomic<int> report_descriptor{-1};
std::atomic<bool> program_ended{false};  // with a session (end)

/// Writes `status` and a newline to the report descriptor, once; only with write(), because the
/// caller may be a signal handler.
void report(int status) {
    const int descriptor = report_descriptor.exchange(-1);
    if (descriptor < 0) return;
    char digits[4];
    int count = 0;
    auto value = static_cast<unsigned>(status);
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < 3);
    char text[5];
    int length = 0;
    while (count > 0) text[length++] = digits[--count];
    text[length++] = '\n';
    static_cast<void>(::write(descriptor, text, static_cast<size_t>(length)));
}

}  // namespace

void report_exit_status_to(int descriptor) {
    report_descriptor.store(descriptor);
}

void report_death_by(int signal) {
    report(128 + signal);
}

void exit(int status) {
    report(status & 0xff);
    ::_exit(status & 0xff);
}

void end(int status) {
    const auto handler = session::end_handler();
    if (handler.function == nullptr) exit(status);
    // Before the other threads hear of it: whichever of them goes back to guest code then stops, and a
    // thread that ends the program as well stops here.
    if (program_ended.exchange(true)) stop();
    report(status & 0xff);
    signals::stop_other_threads();
    // The handler's host calls must not end with EINTR: a runtime signal to this thread waits.
    sigset_t runtime;
    sigemptyset(&runtime);
    sigaddset(&runtime, runtime_signal);
    ::pthread_sigmask(SIG_BLOCK, &runtime, nullptr);
    handler.function(handler.context, status & 0xff);
    stop();
}

bool ended() {
    return program_ended.load();
}

void stop() {
    if (threads::GuestThread *thread = threads::current()) {
        thread->signals.in_runtime.store(signals::runtime_code);
        thread->stopped.store(true);
    }
    sigset_t all;
    sigfillset(&all);
    ::pthread_sigmask(SIG_SETMASK, &all, nullptr);
    for (;;) ::pause();
}

}  // namespace apkrt::process

extern "C" void apkrt_report_exit_status(int descriptor) {
    apkrt::process::report_exit_status_to(descriptor);
}
