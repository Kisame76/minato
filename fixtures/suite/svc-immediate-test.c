// svc-immediate-test: whether the kernel takes the system call number from x8 and ignores the immediate of `svc`.
// Linux on arm64 does (el0_svc reads x8 and never looks at the immediate), so `svc #imm` with x8 = __NR_getpid is
// getpid() for every imm, and `svc #172` with x8 = __NR_getppid is getppid(), not getpid().
// Each svc stands in a function of its own written in assembly: an add copies the number into x8 (not a
// move-immediate) and a return of the function before stands in front of that, so nothing near the svc says
// "syscall" but the word itself; a converter has to take every svc #imm for a syscall on its own account.
// Expected output: fixtures/expected/svc-immediate-test.out
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYSCALL_WITH(name, immediate)                                                         \
    long name(long number);                                                                   \
    __asm__(".text\n.p2align 2\n.type " #name ",%function\n" #name ":\n"                      \
            "    add x8, x0, #0\n    mov x0, #0\n    svc #" #immediate "\n    ret\n")

SYSCALL_WITH(svc_0, 0);
SYSCALL_WITH(svc_1, 1);
SYSCALL_WITH(svc_e4d3, 0xe4d3);
SYSCALL_WITH(svc_ffff, 0xffff);
SYSCALL_WITH(svc_172, 172);

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

int main(void) {
    const long pid = getpid();
    const long parent = getppid();
    printf("svc #0 with x8 = getpid answers the pid: %s\n", yes(svc_0(__NR_getpid) == pid));
    printf("svc #1 with x8 = getpid answers the pid: %s\n", yes(svc_1(__NR_getpid) == pid));
    printf("svc #0xe4d3 with x8 = getpid answers the pid: %s\n", yes(svc_e4d3(__NR_getpid) == pid));
    printf("svc #0xffff with x8 = getpid answers the pid: %s\n", yes(svc_ffff(__NR_getpid) == pid));
    // The immediate is the number of getpid here, the register says getppid: the register wins.
    printf("svc #172 with x8 = getppid answers the parent: %s\n", yes(svc_172(__NR_getppid) == parent));
    printf("the parent is not the pid: %s\n", yes(parent != pid));
    return 0;
}
