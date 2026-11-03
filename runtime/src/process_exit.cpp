#include "process_exit.h"

#include "APKRuntime/apkrt.h"
#include "session.h"
#include "signals.h"
#include "dl.h"
#include "threads.h"

#include <TargetConditionals.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>

namespace apkrt::process {
namespace {

std::atomic<int> report_descriptor{-1};
std::atomic<int> measures_descriptor{-1};
std::atomic<apkrt_log_drain> log_drain{nullptr};
std::atomic<bool> log_drained{false};  // the drain runs once, whichever end reaches it first
std::atomic<bool> program_ended{false};  // with a session (end)

/// Writes `status` and a newline to the report descriptor, once; only with write(), because the
/// caller may be a signal handler.
void report_status(int status) {
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

/// What the measures take at the program's start (program_starts) for its end.
struct ProgramStart {
    std::uint64_t time = 0;         // CLOCK_MONOTONIC, ns
    std::uint64_t process_age = 0;  // ns from the process's start to `time`
    std::uint64_t available = 0;    // os_proc_available_memory, iOS only
};
ProgramStart program_start;
std::atomic<bool> program_started{false};  // program_start is set

/// The measures' lines as text: no snprintf and no malloc, because the caller may be a signal handler.
/// Six lines of 20 digits fit.
struct Lines {
    char text[256];
    std::size_t length = 0;

    void put(const char *part) {
        while (*part != '\0' && length < sizeof text) text[length++] = *part++;
    }

    void put_digits(std::uint64_t value) {
        char digits[20];  // as many as 2^64 - 1 has
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0 && length < sizeof text) text[length++] = digits[--count];
    }

    /// "<name> <bytes>\n".
    void add_bytes(const char *name, std::uint64_t bytes) {
        put(name);
        put(" ");
        put_digits(bytes);
        put("\n");
    }

    /// "<name> <milliseconds>\n" for `nanoseconds`, with one digit after the point, to the nearest tenth.
    void add_milliseconds(const char *name, std::uint64_t nanoseconds) {
        const std::uint64_t tenths = (nanoseconds + 50'000) / 100'000;
        put(name);
        put(" ");
        put_digits(tenths / 10);
        put(".");
        put_digits(tenths % 10);
        put("\n");
    }
};

// The end's memory and text, off the stack, which the guest chose and which may be small: only the caller
// that took the measures' descriptor uses them (report_measures).
task_vm_info_data_t memory_at_end;
Lines measures_text;

/// Writes the measures to their descriptor, once, for a program that started. The clock and task_info are
/// safe in a signal handler; whatever is not, program_starts took before.
void report_measures() {
    const int descriptor = measures_descriptor.exchange(-1);
    if (descriptor < 0 || !program_started.load(std::memory_order_acquire)) return;
    const std::uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    const std::uint64_t run = now > program_start.time ? now - program_start.time : 0;
    // Zeros for what the kernel does not fill: all of it on failure, the later revisions' fields on an older one.
    auto &memory = memory_at_end;
    memory = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (::task_info(::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&memory), &count) != KERN_SUCCESS) {
        memory = {};
    }
    const std::int64_t footprint_peak = memory.ledger_phys_footprint_peak;
    auto &lines = measures_text;
    lines.length = 0;
    lines.add_milliseconds("run-ms", run);
    lines.add_milliseconds("process-ms", program_start.process_age + run);
    lines.add_bytes("footprint-peak", footprint_peak > 0 ? static_cast<std::uint64_t>(footprint_peak) : 0);
    lines.add_bytes("resident-peak", memory.resident_size_peak);
    lines.add_bytes("virtual", memory.virtual_size);
#if TARGET_OS_IPHONE
    lines.add_bytes("available-at-start", program_start.available);
#endif
    static_cast<void>(::write(descriptor, lines.text, lines.length));
}

/// The program's end kinds, which pick the log line below.
enum class EndKind { exited, signaled };

std::atomic<bool> end_logged{false};  // the end goes to the log once, whichever path reaches it first

/// Appends `text` to buf[at..cap), advancing `at`. Signal-safe: no allocation.
void append_text(char *buffer, std::size_t capacity, std::size_t &at, const char *text) {
    while (*text != '\0' && at < capacity) buffer[at++] = *text++;
}

/// Appends the decimal of `value` (>= 0) to buf[at..cap), advancing `at`. Signal-safe.
void append_decimal(char *buffer, std::size_t capacity, std::size_t &at, long value) {
    char digits[20];  // as many as 2^64 - 1 has
    int count = 0;
    auto rest = static_cast<unsigned long>(value);
    do {
        digits[count++] = static_cast<char>('0' + rest % 10);
        rest /= 10;
    } while (rest != 0);
    while (count > 0 && at < capacity) buffer[at++] = digits[--count];
}

/// bionic's name for a Linux signal number (libc/private/bionic_sigdefs.h), for the signal log line.
const char *signal_name(int signal) {
    switch (signal) {
        case 1: return "Hangup";
        case 2: return "Interrupt";
        case 3: return "Quit";
        case 4: return "Illegal instruction";
        case 5: return "Trap";
        case 6: return "Aborted";
        case 7: return "Bus error";
        case 8: return "Floating point exception";
        case 9: return "Killed";
        case 11: return "Segmentation fault";
        case 13: return "Broken pipe";
        case 15: return "Terminated";
        case 31: return "Bad system call";
        default: return "Unknown signal";
    }
}

/// Logs the program's end to stderr once, in `logcat -v tag` form (liblog.cpp), with the line Android's
/// Zygote writes for a child that ended (com_android_internal_os_Zygote.cpp SigChldHandler): "exited
/// cleanly (<status>)" for an exit, "exited due to signal <n> (<name>)" for a signal. Signal-safe: one
/// write() of a stack buffer, no snprintf and no malloc, because the caller may be a signal handler.
void log_end(EndKind kind, int value) {
    // Only an app process gets this line: Android's Zygote logs it for its child app processes, not for a
    // program run from the shell (our NDK fixtures), which begins no session.
    if (session::end_handler().function == nullptr) return;
    if (end_logged.exchange(true)) return;
    char line[128];
    std::size_t at = 0;
    append_text(line, sizeof line, at, "I/Zygote  : Process ");
    append_decimal(line, sizeof line, at, ::getpid());
    if (kind == EndKind::exited) {
        append_text(line, sizeof line, at, " exited cleanly (");
        append_decimal(line, sizeof line, at, value);
        append_text(line, sizeof line, at, ")\n");
    } else {
        append_text(line, sizeof line, at, " exited due to signal ");
        append_decimal(line, sizeof line, at, value);
        append_text(line, sizeof line, at, " (");
        append_text(line, sizeof line, at, signal_name(value));
        append_text(line, sizeof line, at, ")\n");
    }
    static_cast<void>(::write(STDERR_FILENO, line, at));
}

/// The end's report: the log line, the status, then the measures; then the host's log drain, once.
void report(EndKind kind, int value) {
    log_end(kind, value);
    report_status(kind == EndKind::signaled ? 128 + value : value);
    report_measures();
    const apkrt_log_drain drain = log_drain.load();
    if (drain != nullptr && !log_drained.exchange(true)) drain();
}

}  // namespace

void report_exit_status_to(int descriptor) {
    report_descriptor.store(descriptor);
}

void report_exit_measures_to(int descriptor) {
    measures_descriptor.store(descriptor);
}

void program_starts() {
    ProgramStart start;
    start.time = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    // The process's start time is on the wall clock: its age at this moment, which a clock set back since
    // makes 0. The end adds the run's time, so that a clock set during the run moves neither measure.
    const std::uint64_t now = clock_gettime_nsec_np(CLOCK_REALTIME);
    kinfo_proc info{};
    std::size_t size = sizeof info;
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, ::getpid()};
    if (::sysctl(name, 4, &info, &size, nullptr, 0) == 0) {
        const timeval &since = info.kp_proc.p_starttime;
        const std::uint64_t process_start = static_cast<std::uint64_t>(since.tv_sec) * 1'000'000'000 +
                                            static_cast<std::uint64_t>(since.tv_usec) * 1'000;
        start.process_age = now > process_start ? now - process_start : 0;
    }
#if TARGET_OS_IPHONE
    start.available = os_proc_available_memory();
#endif
    program_start = start;
    program_started.store(true, std::memory_order_release);
}

void report_death_by(int signal) {
    report(EndKind::signaled, signal);
}

void exit(int status) {
    report(EndKind::exited, status & 0xff);
    ::_exit(status & 0xff);
}

void end(int status) {
    const auto handler = session::end_handler();
    if (handler.function == nullptr) exit(status);
    // Before the other threads hear of it: whichever of them goes back to guest code then stops, and a
    // thread that ends the program as well stops here.
    if (program_ended.exchange(true)) stop();
    report(EndKind::exited, status & 0xff);
    signals::stop_other_threads();
    session::end();  // the host's waits for answers end, and its posts fail
    // The handler's host calls must not end with EINTR: a runtime signal to this thread waits.
    sigset_t runtime;
    sigemptyset(&runtime);
    sigaddset(&runtime, runtime_signal);
    ::pthread_sigmask(SIG_BLOCK, &runtime, nullptr);
    handler.function(handler.context, status & 0xff);
    stop();
}

void killed() {
    constexpr int sigkill = 9;
    if (session::end_handler().function != nullptr) end(128 + sigkill);
    report_death_by(sigkill);
    ::kill(::getpid(), SIGKILL);
    for (;;) ::pause();
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

extern "C" void apkrt_drain_log_at_end(apkrt_log_drain drain) {
    apkrt::process::log_drain.store(drain);
}

extern "C" void apkrt_report_exit_measures(int descriptor) {
    apkrt::process::report_exit_measures_to(descriptor);
}

namespace apkrt::process {

std::string exit_callers(int status, std::uint64_t frame_pointer, int limit) {
    if (status == 0) return {};
    std::string line = std::format("W/apkrt   : exit_group({}) from ", status);
    std::uint64_t frame = frame_pointer;
    for (int depth = 0; depth < limit && frame != 0 && frame % 8 == 0; depth++) {
        // The guest's frame record {its caller's frame, the return address}, read without a fault: a chain may lead
        // to a page the process cannot read (a guard page, a thread's stack end), where the line ends.
        std::uint64_t record[2] = {0, 0};
        vm_size_t read = 0;  // vm_read_overwrite: iOS's SDK has no mach_vm.h
        if (::vm_read_overwrite(::mach_task_self(), static_cast<vm_address_t>(frame), sizeof record,
                                reinterpret_cast<vm_address_t>(record), &read) != KERN_SUCCESS ||
            read != sizeof record) {
            break;
        }
        const std::uint64_t return_address = record[1];
        if (return_address == 0) break;
        if (depth > 0) line += " <- ";
        // A guest library's symbol through the loader (its Dl_info has the same four fields), else the host's; the
        // call's own instruction, before the return address, which a call that never returns leaves at the next
        // function's start.
        Dl_info info{};
        const auto *address = reinterpret_cast<const void *>(return_address - 1);
        const bool found = dl::name_address(address, &info) != 0 || ::dladdr(address, &info) != 0;
        if (found && info.dli_sname != nullptr) {
            const char *image = info.dli_fname != nullptr ? std::strrchr(info.dli_fname, '/') : nullptr;
            line += std::format("{}+{} ({})", info.dli_sname,
                                return_address - reinterpret_cast<std::uint64_t>(info.dli_saddr),
                                image != nullptr ? image + 1 : (info.dli_fname != nullptr ? info.dli_fname : "?"));
        } else {
            line += std::format("{:#x}", return_address);
        }
        // The chain goes up the stack: a caller's frame lies above, and not far (a broken chain ends here).
        const std::uint64_t caller = record[0];
        if (caller <= frame || caller - frame > (1u << 20)) break;
        frame = caller;
    }
    return line + "\n";
}

void note_exit(int status, std::uint64_t frame_pointer) {
    if (session::end_handler().function == nullptr) return;
    const std::string line = exit_callers(status, frame_pointer);
    static_cast<void>(::write(STDERR_FILENO, line.data(), line.size()));
}

}  // namespace apkrt::process
