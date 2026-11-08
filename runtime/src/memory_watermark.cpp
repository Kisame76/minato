#include "memory_watermark.h"

#include "APKRuntime/apkrt.h"

#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/vm_region.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace apkrt::memory {
namespace {

std::atomic<int> slot{-1};              // the descriptor to write to; below 0 turns the writing off
std::atomic<bool> warning_seen{false};  // a memory-pressure warning has come, and stays seen to the end

constexpr std::size_t value_width = 20;  // the digits of 2^64 - 1: a value is right-justified in this many columns,
                                         // space padded, so every block is the same block_size bytes (the pwrite slot)

/// Writes "<name> " then `value` right-justified in value_width columns (space padded) then "\n" into out[at..size),
/// advancing `at`. No allocation.
void put_value(char *out, std::size_t size, std::size_t &at, const char *name, std::uint64_t value) {
    while (*name != '\0' && at < size) out[at++] = *name++;
    if (at < size) out[at++] = ' ';
    char digits[value_width];
    int count = 0;
    std::uint64_t rest = value;
    do {
        digits[count++] = static_cast<char>('0' + rest % 10);
        rest /= 10;
    } while (rest != 0 && count < static_cast<int>(value_width));
    for (int pad = count; pad < static_cast<int>(value_width) && at < size; pad++) out[at++] = ' ';
    while (count > 0 && at < size) out[at++] = digits[--count];
    if (at < size) out[at++] = '\n';
}

void *periodic_sampler(void *) {
    // A dedicated thread, not a dispatch timer: the sources below share one serial queue, and under the memory
    // pressure we want to record libdispatch may be unable to spawn a worker to run it. A thread created at session
    // start and parked in nanosleep needs no allocation to wake, so it keeps sampling right up to the jetsam kill.
    while (true) {
        const struct timespec delay = {1, 0};  // ~1 Hz; a late wake under load only stretches the interval
        ::nanosleep(&delay, nullptr);
        const int descriptor = slot.load(std::memory_order_relaxed);
        if (descriptor >= 0) write_now(descriptor, false);
        log_instrument(false);  // the 1 Hz memory picture in the session log
    }
    return nullptr;  // one game per process (ADR-0003); the thread runs until the process ends
}

void pressure_fired(void *) {
    const int descriptor = slot.load(std::memory_order_relaxed);
    if (descriptor >= 0) write_now(descriptor, true);  // a memory-pressure warning
    log_instrument(true);
}

/// Starts, once per process (ADR-0003: one game), the 1 Hz sampler thread and the DISPATCH_SOURCE_TYPE_MEMORYPRESSURE
/// source that drive write_now and log_instrument. The thread (not a dispatch timer) survives memory pressure; see
/// periodic_sampler. The pressure source stays for the Mac fixture path, which has no UIKit memory-warning callback.
void ensure_sources() {
    static std::once_flag started;
    std::call_once(started, [] {
        pthread_t thread;
        pthread_attr_t attr;
        if (::pthread_attr_init(&attr) == 0) {
            ::pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            static_cast<void>(::pthread_create(&thread, &attr, periodic_sampler, nullptr));
            ::pthread_attr_destroy(&attr);
        }
        dispatch_queue_t queue = dispatch_queue_create("io.github.kisame76.minato.memory", DISPATCH_QUEUE_SERIAL);
        dispatch_source_t pressure =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                   DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, queue);
        dispatch_source_set_event_handler_f(pressure, pressure_fired);
        dispatch_resume(pressure);  // warn lines for the fixture path; game mode also logs on APKRT_EVENT_LOW_MEMORY
    });
}

}  // namespace

Watermark sample() {
    Watermark watermark;
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (::task_info(::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        watermark.footprint = info.phys_footprint;
        const std::int64_t peak = info.ledger_phys_footprint_peak;
        watermark.peak = peak > static_cast<std::int64_t>(info.phys_footprint) ? static_cast<std::uint64_t>(peak)
                                                                               : info.phys_footprint;
    }
#if TARGET_OS_IPHONE
    watermark.available = os_proc_available_memory();  // 0 on the Mac, which has no such limit
#endif
    watermark.warning = warning_seen.load(std::memory_order_relaxed);
    return watermark;
}

std::size_t format(const Watermark &watermark, char *out, std::size_t size) {
    if (size < block_size) return 0;
    std::size_t at = 0;
    put_value(out, size, at, "footprint", watermark.footprint);
    put_value(out, size, at, "available", watermark.available);
    put_value(out, size, at, "peak", watermark.peak);
    const char *name = "warning";  // a single digit, so not value_width
    while (*name != '\0' && at < size) out[at++] = *name++;
    if (at < size) out[at++] = ' ';
    if (at < size) out[at++] = watermark.warning ? '1' : '0';
    if (at < size) out[at++] = '\n';
    return at;
}

void write_now(int descriptor, bool warning) {
    if (descriptor < 0) return;
    if (warning) warning_seen.store(true, std::memory_order_relaxed);
    const Watermark watermark = sample();
    char block[block_size];
    const std::size_t length = format(watermark, block, sizeof block);
    static_cast<void>(::pwrite(descriptor, block, length, 0));  // the one slot, so the last reading always overwrites
}

void report_to(int descriptor) {
    slot.store(descriptor, std::memory_order_relaxed);
    if (descriptor < 0) return;  // -1 turns the block writing off; the sources, once started, then write no block
    write_now(descriptor, false);  // a first block at once, so even a program that ends within a second leaves one
    ensure_sources();
}

void start_instrument() { ensure_sources(); }

Instrument sample_instrument() {
    Instrument reading;
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (::task_info(::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        reading.footprint = info.phys_footprint;
        reading.internal = info.internal;
        reading.compressed = info.compressed;
        reading.external = info.external;
        // The ledger footprints are signed but never negative; clamp defensively for -Wsign-conversion.
        const auto nonneg = [](std::int64_t value) { return value > 0 ? static_cast<std::uint64_t>(value) : 0; };
        reading.graphics = nonneg(info.ledger_tag_graphics_footprint);
        reading.media = nonneg(info.ledger_tag_media_footprint);
        reading.purgeable = nonneg(info.ledger_purgeable_nonvolatile);
        reading.purgeable_compressed = nonneg(info.ledger_purgeable_novolatile_compressed);
        // "other" closes the line to the footprint: IOKit/GPU-wired, page tables and alternate accounting are
        // counted in phys_footprint but absent from the iOS SDK's task_vm_info, so report them as one remainder.
        const std::uint64_t parts = reading.internal + reading.compressed + reading.graphics + reading.media +
                                    reading.purgeable + reading.purgeable_compressed;
        reading.other = reading.footprint > parts ? reading.footprint - parts : 0;
    }
    // The guest's own mappings carry VM tag 0 (sys_mmap's); split them by backing, as sys_madvise classifies them.
    // vm_region_recurse_64 (not mach_vm_region_recurse, which the iOS SDK lacks): vm_address_t/vm_size_t, as inspect().
    vm_address_t address = 0;
    while (true) {
        vm_size_t size = 0;
        natural_t depth = 0;
        vm_region_submap_short_info_data_64_t region{};
        mach_msg_type_number_t region_count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
        const kern_return_t kr =
            ::vm_region_recurse_64(::mach_task_self(), &address, &size, &depth,
                                   reinterpret_cast<vm_region_recurse_info_t>(&region), &region_count);
        if (kr != KERN_SUCCESS) break;
        if (!region.is_submap && region.user_tag == 0) {
            if (region.external_pager == 0) reading.anon += size;
            else reading.file += size;
        }
        if (address + size <= address) break;  // wrapped past the top of the address space
        address += size;
    }
    return reading;
}

std::size_t format_instrument(const Instrument &reading, bool warning, char *out, std::size_t size) {
    const auto m = [](std::uint64_t bytes) { return static_cast<unsigned long long>(bytes >> 20); };  // bytes -> MiB
    const int length = std::snprintf(
        out, size,
        "I/apkrt   : mem foot=%llu int=%llu comp=%llu gfx=%llu media=%llu purg=%llu purgc=%llu other=%llu ext=%llu "
        "anon=%llu file=%llu%s\n",
        m(reading.footprint), m(reading.internal), m(reading.compressed), m(reading.graphics), m(reading.media),
        m(reading.purgeable), m(reading.purgeable_compressed), m(reading.other), m(reading.external), m(reading.anon),
        m(reading.file), warning ? " warn" : "");
    if (length < 0 || static_cast<std::size_t>(length) >= size) return 0;
    return static_cast<std::size_t>(length);
}

void log_instrument(bool warning) {
    if (::getenv("MINATO_LOADER_LOG") == nullptr) return;
    const Instrument reading = sample_instrument();
    char line[256];
    const std::size_t length = format_instrument(reading, warning, line, sizeof line);
    if (length > 0) static_cast<void>(::write(STDERR_FILENO, line, length));
}

}  // namespace apkrt::memory

extern "C" void apkrt_report_memory_to(int descriptor) { apkrt::memory::report_to(descriptor); }
