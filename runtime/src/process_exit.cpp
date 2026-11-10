#include "process_exit.h"

#include "APKRuntime/apkrt.h"
#include "crash_report.h"
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

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <vector>

namespace apkrt::process {
namespace {

std::atomic<int> report_descriptor{-1};
std::atomic<int> measures_descriptor{-1};
std::atomic<apkrt_log_drain> log_drain{nullptr};
std::atomic<bool> log_drained{false};  // the drain runs once, whichever end reaches it first
std::atomic<bool> program_ended{false};  // with a session (end)

// The crash record a self-kill leaves (note_self_kill), which report_status appends to the status after the bare exit
// number. Built once in normal context; read in report_status with only write(), which a signal handler may reach.
constexpr std::size_t crash_record_capacity = 1024;
char crash_record[crash_record_capacity];
std::size_t crash_record_length = 0;              // set before crash_record_present, read after it
std::atomic<bool> crash_record_present{false};
// One claim for the shared slot: whichever builder (note_self_kill or note_signal) wins fills crash_record; the rest
// return at once, so the record is never overwritten or written by two at the same time (e.g. a self-kill's SIGABRT
// then delivered fatal to note_signal, or a self-kill racing a fault on another thread). The first cause stands.
std::atomic_flag crash_record_claimed = ATOMIC_FLAG_INIT;

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
    // A self-kill's cause (note_self_kill), on the lines after the number, so an app that ended "without a status"
    // shows it. Only write(), so this stays safe on the signal-handler path that reaches report_status.
    if (crash_record_present.load(std::memory_order_acquire)) {
        static_cast<void>(::write(descriptor, crash_record, crash_record_length));
    }
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

/// Appends `value` as lowercase hex (no "0x" prefix) to buf[at..cap), advancing `at`. Signal-safe.
void append_hex(char *buffer, std::size_t capacity, std::size_t &at, std::uint64_t value) {
    char digits[16];  // as many as 2^64 - 1 has in hex
    int count = 0;
    do {
        const auto nibble = static_cast<int>(value & 0xf);
        digits[count++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        value >>= 4;
    } while (value != 0);
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

/// A crash record frame's resolver (note_self_kill): `context` is the loaded guest images, so a return address names
/// the library and its guest offset under crash_report's privacy mask. A rewriter stub lies below every image, so a
/// stub address is in no image and resolves to a bare "0x<address>", never a foreign .so offset (container_format.h).
crash::Module resolve_in_mapped_objects(std::uint64_t address, void *context) {
    const auto &objects = *static_cast<const std::vector<crash::LoadedObject> *>(context);
    return crash::resolve_in_objects(address, objects);
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

void note_self_kill(int signal, std::uint64_t caller_fp) {
    // One claim across both record builders (self-kill and fatal signal): the first fills crash_record, the rest return
    // at once, so no one writes the shared buffer under another. crash_record_present then tells report_status it is
    // ready.
    if (crash_record_claimed.test_and_set(std::memory_order_relaxed)) return;  // the first cause's record stands
    // The loaded guest images, so a caller frame names <library>+<guest offset> under the privacy mask; taken here in
    // the syscall's normal context, where allocation is allowed. The sonames point into `mapped`, which outlives the
    // backtrace, and write_backtrace copies the names it keeps into crash_record.
    std::vector<dl::MappedObject> mapped = dl::mapped_objects();
    std::vector<crash::LoadedObject> objects;
    objects.reserve(mapped.size());
    for (const dl::MappedObject &object : mapped) {
        const std::string_view path = object.path;
        const std::size_t slash = path.rfind('/');
        objects.push_back({.image_begin = object.image_begin,
                           .image_end = object.image_end,
                           .bias = object.bias,
                           .soname = slash == std::string_view::npos ? path : path.substr(slash + 1)});
    }
    std::size_t at = 0;
    append_text(crash_record, crash_record_capacity, at, "killed itself with signal ");
    append_decimal(crash_record, crash_record_capacity, at, signal);
    append_text(crash_record, crash_record_capacity, at, " (");
    append_text(crash_record, crash_record_capacity, at, signal_name(signal));
    append_text(crash_record, crash_record_capacity, at, ")\n");
    crash::write_backtrace(crash_record, crash_record_capacity, at, caller_fp, 8, resolve_in_mapped_objects, &objects);
    crash_record_length = at;
    crash_record_present.store(true, std::memory_order_release);
}

void note_signal(int signal, int code, std::uint64_t fault_address, std::uint64_t pc, std::uint64_t lr,
                 std::uint64_t fp) {
    // The fatal-signal record, built at the point the handler is about to die_by. ASYNC-SIGNAL-SAFE: no allocation and
    // no lock -- resolve_registered reads the lock-free module registry, so a frame still names <library>+<guest
    // offset> under the privacy mask without dl's allocating object list. The first cause wins the shared slot (a prior
    // self-kill's record is NOT overwritten by the delivered fatal signal); a racing one returns. report_status appends
    // the stash after the bare 128 + signal number.
    if (crash_record_claimed.test_and_set(std::memory_order_relaxed)) return;
    std::size_t at = 0;
    append_text(crash_record, crash_record_capacity, at, "signal: ");
    append_decimal(crash_record, crash_record_capacity, at, signal);
    append_text(crash_record, crash_record_capacity, at, " (");
    append_text(crash_record, crash_record_capacity, at, signal_name(signal));
    append_text(crash_record, crash_record_capacity, at, ")\ncode: ");
    append_decimal(crash_record, crash_record_capacity, at, code);
    append_text(crash_record, crash_record_capacity, at, "\nfault: 0x");
    append_hex(crash_record, crash_record_capacity, at, fault_address);
    append_text(crash_record, crash_record_capacity, at, "\npc: ");
    crash::format_frame(crash_record, crash_record_capacity, at, pc, crash::resolve_registered(pc, nullptr));
    append_text(crash_record, crash_record_capacity, at, "\nlr: ");
    crash::format_frame(crash_record, crash_record_capacity, at, lr, crash::resolve_registered(lr, nullptr));
    append_text(crash_record, crash_record_capacity, at, "\n");
    crash::write_backtrace(crash_record, crash_record_capacity, at, fp, 8, crash::resolve_registered, nullptr);
    crash_record_length = at;
    crash_record_present.store(true, std::memory_order_release);
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

namespace {

constexpr std::size_t failed_path_slots = 16;
constexpr std::size_t failed_path_length = 159;

struct FailedPath {
    std::string syscall;
    std::string path;
    int error = 0;
};

std::mutex failed_paths_mutex;
FailedPath failed_paths[failed_path_slots];  // guarded by failed_paths_mutex; a ring
std::size_t failed_path_count = 0;           // how many were kept in all

const char *error_name(int error) {
    switch (error) {
        case 2: return "ENOENT";
        case 13: return "EACCES";
        case 20: return "ENOTDIR";
        default: return nullptr;
    }
}

}  // namespace

void note_failed_path(const char *syscall, const std::string &path, int error) {
    if (error_name(error) == nullptr) return;
    const std::string kept = path.substr(0, failed_path_length);
    const std::lock_guard lock(failed_paths_mutex);
    if (failed_path_count > 0) {
        const FailedPath &last = failed_paths[(failed_path_count - 1) % failed_path_slots];
        if (last.error == error && last.syscall == syscall && last.path == kept) return;
    }
    failed_paths[failed_path_count % failed_path_slots] = {syscall, kept, error};
    failed_path_count++;
}

std::string failed_paths_lines() {
    const std::lock_guard lock(failed_paths_mutex);
    std::string lines;
    const std::size_t first = failed_path_count > failed_path_slots ? failed_path_count - failed_path_slots : 0;
    for (std::size_t index = first; index < failed_path_count; index++) {
        const FailedPath &failed = failed_paths[index % failed_path_slots];
        lines += std::format("W/apkrt   : failed path: {} {} {}\n", failed.syscall, failed.path,
                             error_name(failed.error));
    }
    return lines;
}

namespace {

constexpr std::size_t output_size = 4096;

std::mutex output_mutex;
char output[output_size];       // guarded by output_mutex; a ring of the last bytes written
std::size_t output_total = 0;   // how many bytes were written in all

}  // namespace

namespace {

struct LineState {
    bool at_start = true;   // the next byte begins a line
    bool skipping = false;  // the line in progress is one of liblog's
};
LineState line_states[2];  // descriptors 1 and 2; guarded by output_mutex

/// Whether `text` (the part of a line seen so far) is one of Minato's liblog lines, "<V|D|I|W|E|F>/<tag>: <message>"
/// (runtime/bionic/src/liblog.cpp): the runtime's own words, which must not push the app's out of the ring.
/// Named limit: an app's own line of that shape ("X/tag: msg") is dropped too.
bool is_log_line(const char *text, std::size_t length) {
    if (length < 4 || text[1] != '/' || std::strchr("VDIWEF", text[0]) == nullptr) return false;
    return std::string_view(text, length).find(": ", 2) != std::string_view::npos;
}

void keep_output(const char *data, std::size_t length) {
    if (length > output_size) {
        data += length - output_size;
        length = output_size;
    }
    for (std::size_t index = 0; index < length; index++) output[(output_total + index) % output_size] = data[index];
    output_total += length;
}

}  // namespace

void note_output(int fd, const char *data, std::size_t length) {
    if ((fd != 1 && fd != 2) || data == nullptr || length == 0) return;
    const std::lock_guard lock(output_mutex);
    LineState &state = line_states[fd - 1];
    const char *const end = data + length;
    while (data < end) {
        const char *newline = static_cast<const char *>(std::memchr(data, '\n', static_cast<std::size_t>(end - data)));
        const char *stop = newline != nullptr ? newline + 1 : end;
        if (state.at_start) state.skipping = is_log_line(data, static_cast<std::size_t>(stop - data));
        if (!state.skipping) keep_output(data, static_cast<std::size_t>(stop - data));
        state.at_start = newline != nullptr;
        data = stop;
    }
}

std::string output_lines() {
    std::string text;
    {
        const std::lock_guard lock(output_mutex);
        const std::size_t kept = std::min(output_total, output_size);
        for (std::size_t index = output_total - kept; index < output_total; index++) {
            text += output[index % output_size];
        }
        // Cut by the ring: the first line is a piece of one, and is dropped.
        if (output_total > output_size) {
            const auto newline = text.find('\n');
            text = newline == std::string::npos ? std::string() : text.substr(newline + 1);
        }
    }
    std::string lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line;
        for (std::size_t index = start; index < end; index++) {
            const unsigned char byte = static_cast<unsigned char>(text[index]);
            if (byte == '\r') continue;
            line += (byte >= 0x20 && byte < 0x7f) || byte == '\t' ? static_cast<char>(byte) : '?';
        }
        if (!line.empty()) lines += "W/apkrt   : stderr: " + line + "\n";
        start = end + 1;
    }
    return lines;
}

void forget_output() {
    const std::lock_guard lock(output_mutex);
    output_total = 0;
    line_states[0] = line_states[1] = LineState();
}

void forget_failed_paths() {
    const std::lock_guard lock(failed_paths_mutex);
    failed_path_count = 0;
}

void note_exit(int status, std::uint64_t frame_pointer) {
    if (session::end_handler().function == nullptr) return;
    std::string line = exit_callers(status, frame_pointer);
    if (status != 0) line += failed_paths_lines() + output_lines();
    static_cast<void>(::write(STDERR_FILENO, line.data(), line.size()));
}

}  // namespace apkrt::process
