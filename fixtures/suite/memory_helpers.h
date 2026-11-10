// The helpers of the memory fixtures that compare how memory is released with Android 15 (memory-release-test,
// memory-munmap-piece-test, memory-remove-memfd-test, memory-pageout-test): the process's resident size from
// /proc/self/stat (field 24, which Android and Minato both give), a touch of every page, the name of an errno, whether
// pages left the resident size, and a read that survives a fault.
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_REMOVE
#define MADV_REMOVE 9
#endif
#ifndef MADV_COLD
#define MADV_COLD 20
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif

static const size_t kMiB = 1 << 20;

// The process's resident size in MiB.
static inline long rss(void) {
    char text[1024];
    FILE *file = fopen("/proc/self/stat", "r");
    if (file == NULL) return -1;
    size_t size = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[size] = 0;
    const char *after_name = strrchr(text, ')');
    long pages = -1;
    if (after_name == NULL ||
        sscanf(after_name + 1,
               " %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %*lu %*lu %*ld %*ld %*ld %*ld %*ld %*ld %*llu %*lu"
               " %ld", &pages) != 1) {
        return -1;
    }
    return (long)((unsigned long)pages * (unsigned long)sysconf(_SC_PAGESIZE) / kMiB);
}

static inline void touch(void *memory, size_t size) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t at = 0; at < size; at += page) ((volatile unsigned char *)memory)[at] = (unsigned char)(at >> 12);
}

static inline const char *name_of(int error) {
    switch (error) {
    case 0: return "ok";
    case EINVAL: return "EINVAL";
    case ENOSYS: return "ENOSYS";
    case EOPNOTSUPP: return "EOPNOTSUPP";
    case ENOMEM: return "ENOMEM";
    case EPERM: return "EPERM";
    case EACCES: return "EACCES";
    case EBADF: return "EBADF";
    case EEXIST: return "EEXIST";
    default: return "other";
    }
}

// Whether the pages of `base` went out of the resident size: under 32 MiB of what it was before they were touched.
static inline const char *left(long before, long after) {
    return after - before < 32 ? "left the resident size" : "stay resident";
}

static sigjmp_buf jump;

static inline void on_fault(int signal_number) {
    (void)signal_number;
    siglongjmp(jump, 1);
}

// Whether the byte at `address` can be read.
static inline int readable(const volatile unsigned char *address) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_fault;
    action.sa_flags = SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    if (sigsetjmp(jump, 1) == 0) {
        (void)*address;
        return 1;
    }
    return 0;
}

static inline void *anonymous(size_t size, int protection, int extra) {
    return mmap(NULL, size, protection, MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
}

