// kernel-test: what ART's start asks of the kernel and of the files of /proc and /sys (ADR-0033,
// decision 8), through bionic as an app's code reaches it: uname, init's environment (ADR-0039,
// decision 2), the main thread's stack from /proc/self/stat and /proc/self/maps, realpath through
// /proc/self/fd, the CPU count, priorities, affinity and capabilities, and FUTEX_REQUEUE as ART's
// condition variables use it. Every line is the same on any arm64 device with Android 15.
// With the argument "maps" it prints its /proc/self/maps instead, to compare the format by eye.
// Expected output: fixtures/expected/kernel-test.out
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/capability.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

static void kernel(void) {
    struct utsname names;
    int major = 0, minor = 0;
    uname(&names);
    sscanf(names.release, "%d.%d", &major, &minor);
    printf("uname: %s %s, release 5.13 or newer: %s\n", names.sysname, names.machine,
           yes(major > 5 || (major == 5 && minor >= 13)));
}

static void environment(void) {
    static const char *const names[] = {"ANDROID_BOOTLOGO", "ANDROID_ROOT", "ANDROID_ASSETS", "ANDROID_DATA",
                                        "ANDROID_STORAGE", "ANDROID_ART_ROOT", "ANDROID_I18N_ROOT",
                                        "ANDROID_TZDATA_ROOT", "EXTERNAL_STORAGE", "ASEC_MOUNTPOINT"};
    for (size_t index = 0; index < sizeof names / sizeof names[0]; ++index) {
        const char *value = getenv(names[index]);
        printf("environment: %s=%s\n", names[index], value != NULL ? value : "(unset)");
    }
}

/// The main thread's stack as bionic's pthread_getattr_np finds it, from RLIMIT_STACK, the startstack
/// field of /proc/self/stat and /proc/self/maps; then those two files by themselves.
static void main_stack(void) {
    pthread_attr_t attributes;
    void *base = NULL;
    size_t size = 0;
    int here = 0;
    pthread_getattr_np(pthread_self(), &attributes);
    pthread_attr_getstack(&attributes, &base, &size);
    const uintptr_t low = (uintptr_t)base, high = low + size, sp = (uintptr_t)&here;
    printf("main thread stack: %zu bytes, holds the stack pointer: %s\n", size, yes(low <= sp && sp < high));

    char line[1024] = "";
    FILE *stat = fopen("/proc/self/stat", "re");
    if (stat != NULL) {
        fgets(line, sizeof line, stat);
        fclose(stat);
    }
    const char *comm_end = strrchr(line, ')');
    const char *comm_begin = strchr(line, '(');
    unsigned long long start_stack = 0;
    int fields = 0;
    if (comm_begin != NULL && comm_end != NULL) {
        fields = 2;
        for (const char *at = comm_end + 1; *at != '\0' && *at != '\n'; ++at) {
            if (at[0] == ' ' && at[1] != ' ' && at[1] != '\n') ++fields;
        }
        sscanf(comm_end + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u "
                             "%*d %*u %*u %*u %llu", &start_stack);
    }
    printf("/proc/self/stat: comm %.*s, %d fields, startstack in the main stack: %s\n",
           comm_begin != NULL && comm_end != NULL ? (int)(comm_end - comm_begin - 1) : 0,
           comm_begin != NULL ? comm_begin + 1 : "", fields, yes(low <= start_stack && start_stack < high));

    // Every line in Linux's format, a name where there is one at column 73; the stack, the program's code
    // and libc under their names.
    FILE *maps = fopen("/proc/self/maps", "re");
    int lines = 0, parsed = 0, names_placed = 1, stack_named = 0, program_named = 0;
    char libc[PATH_MAX] = "(none)";
    while (maps != NULL && fgets(line, sizeof line, maps) != NULL) {
        unsigned long long begin, end, offset, inode;
        char permissions[5];
        unsigned device_major, device_minor;
        int prefix = 0;
        ++lines;
        if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &begin, &end, permissions, &offset, &device_major,
                   &device_minor, &inode, &prefix) != 7) {
            continue;
        }
        ++parsed;
        line[strcspn(line, "\n")] = '\0';
        if (line[prefix] == '\0') continue;  // anonymous
        if (prefix > 73 || line[72] != ' ' || line[73] == ' ') names_placed = 0;
        const char *name = line + prefix;
        if (strcmp(name, "[stack]") == 0 && begin <= sp && sp < end) stack_named = 1;
        if (strcmp(name, "/data/local/tmp/kernel-test") == 0 && permissions[2] == 'x') program_named = 1;
        const size_t length = strlen(name);
        if (length >= 8 && strcmp(name + length - 8, "/libc.so") == 0 && permissions[2] == 'x') {
            snprintf(libc, sizeof libc, "%s", name);
        }
    }
    if (maps != NULL) fclose(maps);
    printf("/proc/self/maps: every line parses: %s, names at column 73: %s\n", yes(lines > 0 && parsed == lines),
           yes(names_placed));
    printf("/proc/self/maps: [stack] holds the stack pointer: %s, the program's code is named: %s\n",
           yes(stack_named), yes(program_named));
    printf("/proc/self/maps: libc's code: %s\n", libc);
}

/// bionic's realpath opens the path and reads the link /proc/self/fd/<n>.
static void links(void) {
    char resolved[PATH_MAX];
    FILE *file = fopen("/data/local/tmp/kernel-test-file", "we");
    if (file != NULL) fclose(file);
    printf("realpath: %s\n",
           realpath("/data/local/tmp/./kernel-test-file", resolved) != NULL ? resolved : strerror(errno));
    unlink("/data/local/tmp/kernel-test-file");
    int ends[2];
    char target[64] = "";
    char link[64];
    pipe(ends);
    snprintf(link, sizeof link, "/proc/self/fd/%d", ends[0]);
    const ssize_t length = readlink(link, target, sizeof target - 1);
    if (length > 0) target[length] = '\0';
    printf("/proc/self/fd: a pipe's link starts with pipe:[ and ends with ]: %s\n",
           yes(strncmp(target, "pipe:[", 6) == 0 && target[strlen(target) - 1] == ']'));
    close(ends[0]);
    close(ends[1]);
}

/// The processors that sysconf counts from /sys/devices/system/cpu and the ones sched_getaffinity
/// gives; then what Unity and libcap call, before the affinity changes.
static void processors(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    const long configured = sysconf(_SC_NPROCESSORS_CONF), online = sysconf(_SC_NPROCESSORS_ONLN);
    printf("processors: configured, online and in the affinity agree: %s, more than one: %s\n",
           yes(configured == online && online == CPU_COUNT(&set)), yes(online > 1));

    printf("setpriority: %d\n", setpriority(PRIO_PROCESS, 0, 10));
    cpu_set_t first;
    CPU_ZERO(&first);
    CPU_SET(0, &first);
    printf("sched_setaffinity: %d\n", sched_setaffinity(0, sizeof first, &first));
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct sets[2];
    printf("capget: %ld\n", syscall(SYS_capget, &header, sets));
}

static int condition, mutex;

static long futex(int *word, int operation, int value, uintptr_t count, int *other) {
    return syscall(SYS_futex, word, operation | FUTEX_PRIVATE_FLAG, value, count, other, 0);
}

static void *wait_on_condition(void *result) {
    *(long *)result = futex(&condition, FUTEX_WAIT, 0, 0, NULL);
    return NULL;
}

/// ART's ConditionVariable: Signal and Broadcast move the waiters onto the mutex's word.
static void requeue(void) {
    pthread_t threads[2];
    long results[2] = {-1, -1};
    for (int index = 0; index < 2; ++index) pthread_create(&threads[index], NULL, wait_on_condition, &results[index]);
    long moved = 0;
    for (int attempt = 0; attempt < 1000 && moved < 2; ++attempt) {
        moved += futex(&condition, FUTEX_REQUEUE, 0, 1, &mutex);
        if (moved < 2) usleep(1000);
    }
    const long both = futex(&mutex, FUTEX_REQUEUE, 1, INT_MAX, &condition);
    const long rest = futex(&condition, FUTEX_WAKE, INT_MAX, 0, NULL);
    for (int index = 0; index < 2; ++index) pthread_join(threads[index], NULL);
    printf("futex requeue: moved %ld one by one, then woke and moved %ld, then woke %ld; the waits gave %ld and %ld\n",
           moved, both, rest, results[0], results[1]);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "maps") == 0) {
        FILE *maps = fopen("/proc/self/maps", "re");
        for (int c; maps != NULL && (c = fgetc(maps)) != EOF;) putchar(c);
        return 0;
    }
    kernel();
    environment();
    main_stack();
    links();
    processors();
    requeue();
    return 0;
}
