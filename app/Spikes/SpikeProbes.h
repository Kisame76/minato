// Probes for the feasibility spikes S1b, S2 and S3 (docs/STATUS.md). Only Debug builds run them, when the
// app is launched with -runSpikes (tools/spikes.sh).

#ifndef SPIKE_PROBES_H
#define SPIKE_PROBES_H

#include <stdint.h>

/// S1b: the two kernel checks dyld makes before it maps a dylib (LiveContainer's JIT-less setup check
/// makes them too). F_ADDFILESIGS_RETURN registers the file's code signature, F_CHECK_LV asks whether
/// library validation lets this process map the file.
typedef struct {
    int open_errno;               // 0 once the file is open and its LC_CODE_SIGNATURE is found
    int add_signatures;           // fcntl(F_ADDFILESIGS_RETURN), -1 on failure
    int add_signatures_errno;
    int library_validation;       // fcntl(F_CHECK_LV), -1 on failure
    int library_validation_errno;
    char library_validation_message[256];
} spike_signature_check;

/// `path` must be a thin 64-bit Mach-O, like the test dylib.
spike_signature_check spike_s1b_check_signature(const char *path);

/// S2: how much memory the process may still use before jetsam ends it, and what it uses now.
typedef struct {
    uint64_t available;       // os_proc_available_memory()
    uint64_t footprint;       // TASK_VM_INFO phys_footprint
    uint64_t limit_remaining; // TASK_VM_INFO limit_bytes_remaining
    uint64_t physical;        // hw.memsize
    uint64_t page_size;       // vm_page_size
} spike_memory;

spike_memory spike_s2_memory(void);

/// S2: the largest single PROT_NONE reservation, and all reservations added up until the address
/// space is full (gaps below four times the granularity are left for other threads).
typedef struct {
    uint64_t largest;
    uint64_t total;
    uint64_t regions;
    uint64_t highest_end;
} spike_address_space;

spike_address_space spike_s2_address_space(uint64_t granularity);

/// S3: a register after writing it and then making a syscall, blocking, and being preempted.
typedef struct {
    uint64_t initial;       // value found before writing
    uint64_t written;
    uint64_t immediately;   // read right after writing
    uint64_t after_syscall; // after getpid through svc #0x80
    uint64_t after_sleep;   // after usleep, which blocks the thread
    uint64_t after_spin;    // after spinning 200 ms without syscalls, so timer interrupts preempt it
} spike_register_trace;

/// Each trace runs on a fresh thread and restores the register afterwards.
spike_register_trace spike_s3_tpidr_el0(void);
spike_register_trace spike_s3_x18(void);
uint64_t spike_s3_tpidrro_el0(void);

#endif
