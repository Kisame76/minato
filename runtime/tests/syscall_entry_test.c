// Checks the Linux syscall entry that every svc veneer calls (ADR-0019): like the Linux kernel it
// returns the result in x0 and keeps every other register, the flags and FPSR. Guest code may keep
// values in any register across an inline svc, so a single clobbered register would be a bug that
// shows up far away from its cause.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct probe_state {
    uint64_t x[32];
    uint64_t nzcv;
    uint64_t fpsr;
    __uint128_t q[32];
};
_Static_assert(__builtin_offsetof(struct probe_state, nzcv) == 256, "layout of syscall_entry_probe.S");
_Static_assert(__builtin_offsetof(struct probe_state, q) == 272, "layout of syscall_entry_probe.S");

void probe_syscall_entry(uint64_t number, const struct probe_state *before, struct probe_state *after);

// Linux AArch64 syscall numbers (asm-generic/unistd.h) and errno values (asm-generic/errno*.h).
enum { linux_write = 64, linux_getpid = 172 };
enum { linux_ebadf = 9, linux_enosys = 38 };

static int failures = 0;

#define CHECK(condition) check((condition), #condition, __LINE__)

static void check(int condition, const char *expression, int line) {
    if (condition) return;
    fprintf(stderr, "syscall_entry_test.c:%d: CHECK failed: %s\n", line, expression);
    ++failures;
}

/// A state where every register holds a different value, so a swap shows up as well as a clobber.
static struct probe_state distinct_state(void) {
    struct probe_state state;
    memset(&state, 0, sizeof state);
    for (int index = 0; index < 32; ++index) {
        state.x[index] = 0x0101010101010101ULL * (uint64_t)(index + 1) ^ 0x8000000000000000ULL;
        state.q[index] = ((__uint128_t)(0xa5a5a5a5a5a5a5a5ULL + (uint64_t)index) << 64) | (uint64_t)(index * 977 + 1);
    }
    state.nzcv = 0x60000000;  // Z and C set, N and V clear
    state.fpsr = 0x0800009f;  // QC and every cumulative exception flag
    return state;
}

/// Everything except x0 must come back unchanged. x16 and x30 belong to the veneer, x18 to XNU.
static void check_kept(const struct probe_state *before, const struct probe_state *after, const char *what) {
    for (int index = 1; index < 30; ++index) {
        if (index == 16 || index == 18) continue;
        if (before->x[index] != after->x[index]) {
            fprintf(stderr, "%s: x%d changed from %#llx to %#llx\n", what, index,
                    (unsigned long long)before->x[index], (unsigned long long)after->x[index]);
            ++failures;
        }
    }
    for (int index = 0; index < 32; ++index) {
        if (before->q[index] != after->q[index]) {
            fprintf(stderr, "%s: q%d changed\n", what, index);
            ++failures;
        }
    }
    if (before->nzcv != after->nzcv) {
        fprintf(stderr, "%s: NZCV changed from %#llx to %#llx\n", what, (unsigned long long)before->nzcv,
                (unsigned long long)after->nzcv);
        ++failures;
    }
    if (before->fpsr != after->fpsr) {
        fprintf(stderr, "%s: FPSR changed from %#llx to %#llx\n", what, (unsigned long long)before->fpsr,
                (unsigned long long)after->fpsr);
        ++failures;
    }
}

static int64_t call(uint64_t number, struct probe_state *before, const char *what) {
    before->x[8] = number;
    struct probe_state after;
    memset(&after, 0xee, sizeof after);
    probe_syscall_entry(number, before, &after);
    check_kept(before, &after, what);
    return (int64_t)after.x[0];
}

int main(void) {
    struct probe_state state = distinct_state();
    CHECK(call(linux_getpid, &state, "getpid") == getpid());

    // The opposite flags as well: a stray compare in the entry can reproduce any single pattern.
    state = distinct_state();
    state.nzcv = 0x90000000;  // N and V set, Z and C clear
    state.fpsr = 0;
    CHECK(call(linux_getpid, &state, "getpid with other flags") == getpid());

    int fds[2];
    CHECK(pipe(fds) == 0);
    static const char message[] = "through the syscall entry";
    state = distinct_state();
    state.x[0] = (uint64_t)fds[1];
    state.x[1] = (uint64_t)(uintptr_t)message;
    state.x[2] = sizeof message - 1;
    CHECK(call(linux_write, &state, "write") == (int64_t)(sizeof message - 1));
    char received[sizeof message] = {0};
    CHECK(read(fds[0], received, sizeof received) == (ssize_t)(sizeof message - 1));
    CHECK(strcmp(received, message) == 0);

    // Errors come back as negative Linux errno values, not Darwin ones.
    state = distinct_state();
    state.x[0] = 999;
    state.x[1] = (uint64_t)(uintptr_t)message;
    state.x[2] = 1;
    CHECK(call(linux_write, &state, "write to a closed descriptor") == -linux_ebadf);

    state = distinct_state();
    CHECK(call(0x7fff, &state, "unknown syscall") == -linux_enosys);

    if (failures == 0) printf("ok   syscall entry keeps every register but x0\n");
    return failures == 0 ? 0 : 1;
}
