#include "process_exit.h"

#include "APKRuntime/apkrt.h"

#include <unistd.h>

#include <atomic>

namespace apkrt::process {
namespace {

std::atomic<int> report_descriptor{-1};

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

}  // namespace apkrt::process

extern "C" void apkrt_report_exit_status(int descriptor) {
    apkrt::process::report_exit_status_to(descriptor);
}
