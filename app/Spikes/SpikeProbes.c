#include "SpikeProbes.h"

#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

// MARK: - S1b: code signature checks before mapping

spike_signature_check spike_s1b_check_signature(const char *path) {
    spike_signature_check result = {0};
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        result.open_errno = errno;
        return result;
    }
    struct mach_header_64 header;
    struct linkedit_data_command signature = {0};
    if (pread(fd, &header, sizeof header, 0) == sizeof header && header.magic == MH_MAGIC_64) {
        off_t offset = sizeof header;
        for (uint32_t i = 0; i < header.ncmds; i++) {
            struct load_command command;
            if (pread(fd, &command, sizeof command, offset) != sizeof command || command.cmdsize < sizeof command) break;
            if (command.cmd == LC_CODE_SIGNATURE) {
                pread(fd, &signature, sizeof signature, offset);
                break;
            }
            offset += command.cmdsize;
        }
    }
    if (signature.cmd != LC_CODE_SIGNATURE) {
        result.open_errno = EBADMACHO;
        close(fd);
        return result;
    }

    // Thin file, so the slice starts at 0. For F_ADDFILESIGS* fs_blob_start is a file offset, as in dyld.
    fsignatures_t signatures = {0};
    signatures.fs_file_start = 0;
    signatures.fs_blob_start = (void *)(uintptr_t)signature.dataoff;
    signatures.fs_blob_size = signature.datasize;
    result.add_signatures = fcntl(fd, F_ADDFILESIGS_RETURN, &signatures);
    if (result.add_signatures == -1) result.add_signatures_errno = errno;

    fchecklv_t check = {0};
    check.lv_file_start = 0;
    check.lv_error_message_size = sizeof result.library_validation_message;
    check.lv_error_message = result.library_validation_message;
    result.library_validation = fcntl(fd, F_CHECK_LV, &check);
    if (result.library_validation == -1) result.library_validation_errno = errno;
    close(fd);
    return result;
}

// MARK: - S2: memory and address space

spike_memory spike_s2_memory(void) {
    spike_memory result = {0};
    result.available = os_proc_available_memory();
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        result.footprint = info.phys_footprint;
        result.limit_remaining = info.limit_bytes_remaining;
    }
    size_t size = sizeof(result.physical);
    sysctlbyname("hw.memsize", &result.physical, &size, NULL, 0);
    result.page_size = vm_page_size;
    return result;
}

/// Binary search for the largest reservation that still succeeds, to `granularity` precision.
static uint64_t largest_reservation(uint64_t granularity, uint64_t upper) {
    uint64_t lo = 0, hi = upper;
    while (hi - lo > granularity) {
        uint64_t mid = lo + (hi - lo) / 2;
        mid -= mid % granularity;
        if (mid <= lo) break;
        void *region = mmap(NULL, mid, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (region == MAP_FAILED) {
            hi = mid;
        } else {
            munmap(region, mid);
            lo = mid;
        }
    }
    return lo;
}

spike_address_space spike_s2_address_space(uint64_t granularity) {
    enum { max_regions = 1024 };
    void *regions[max_regions];
    uint64_t sizes[max_regions];
    spike_address_space result = {0};
    const uint64_t upper = 1ull << 40; // 1 TiB, far above anything iOS grants an app

    while (result.regions < max_regions) {
        uint64_t size = largest_reservation(granularity, upper);
        if (size < 4 * granularity) break;
        void *region = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (region == MAP_FAILED) break;
        if (result.regions == 0) result.largest = size;
        regions[result.regions] = region;
        sizes[result.regions] = size;
        result.regions++;
        result.total += size;
        uint64_t end = (uint64_t)(uintptr_t)region + size;
        if (end > result.highest_end) result.highest_end = end;
    }
    for (uint64_t i = 0; i < result.regions; i++) {
        munmap(regions[i], sizes[i]);
    }
    return result;
}

// MARK: - S3: TPIDR_EL0 and x18

static inline uint64_t read_tpidr_el0(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

static inline void write_tpidr_el0(uint64_t value) {
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(value));
}

// The compiler never allocates x18 on Darwin, so it keeps its value between these statements
// unless the kernel changes it.
static inline uint64_t read_x18(void) {
    uint64_t value;
    __asm__ volatile("mov %0, x18" : "=r"(value));
    return value;
}

static inline void write_x18(uint64_t value) {
    __asm__ volatile("mov x18, %0" : : "r"(value));
}

uint64_t spike_s3_tpidrro_el0(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value;
}

/// getpid straight through the syscall instruction, without libc in between.
static inline void raw_getpid(void) {
    register uint64_t number __asm__("x16") = 20; // SYS_getpid
    register uint64_t result __asm__("x0");
    __asm__ volatile("svc #0x80" : "=r"(result), "+r"(number) : : "x1", "memory", "cc");
    (void)result;
}

/// Busy loop on the commpage clock, so no syscall happens while spinning.
static void spin(uint64_t milliseconds) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    uint64_t ticks = milliseconds * 1000000ull * timebase.denom / timebase.numer;
    uint64_t end = mach_absolute_time() + ticks;
    while (mach_absolute_time() < end) {
        __asm__ volatile("" : : : "memory");
    }
}

// The low bits stay zero: libsystem may read the CPU number from TPIDR_EL0 and index per-CPU data with it.
static const uint64_t marker = 0x5A5A5A5A5A5A0000ull;

static void *trace_tpidr_el0(void *context) {
    spike_register_trace *trace = context;
    trace->initial = read_tpidr_el0();
    trace->written = marker;
    write_tpidr_el0(marker);
    trace->immediately = read_tpidr_el0();
    write_tpidr_el0(marker);
    raw_getpid();
    trace->after_syscall = read_tpidr_el0();
    write_tpidr_el0(marker);
    usleep(20000);
    trace->after_sleep = read_tpidr_el0();
    write_tpidr_el0(marker);
    spin(200);
    trace->after_spin = read_tpidr_el0();
    write_tpidr_el0(trace->initial);
    return NULL;
}

static void *trace_x18(void *context) {
    spike_register_trace *trace = context;
    trace->initial = read_x18();
    trace->written = marker;
    write_x18(marker);
    trace->immediately = read_x18();
    write_x18(marker);
    raw_getpid();
    trace->after_syscall = read_x18();
    write_x18(marker);
    usleep(20000);
    trace->after_sleep = read_x18();
    write_x18(marker);
    spin(200);
    trace->after_spin = read_x18();
    write_x18(trace->initial);
    return NULL;
}

static spike_register_trace run_on_fresh_thread(void *(*body)(void *)) {
    spike_register_trace trace = {0};
    pthread_t thread;
    if (pthread_create(&thread, NULL, body, &trace) == 0) {
        pthread_join(thread, NULL);
    }
    return trace;
}

spike_register_trace spike_s3_tpidr_el0(void) {
    return run_on_fresh_thread(trace_tpidr_el0);
}

spike_register_trace spike_s3_x18(void) {
    return run_on_fresh_thread(trace_x18);
}
