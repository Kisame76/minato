// sched-priority-test: sched_get_priority_max and sched_get_priority_min for every scheduling policy, as engines that
// size their thread priorities from them (LuaJIT's host, SDL, OpenAL) ask. Expected output:
// fixtures/expected/sched-priority-test.out
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static void show(int policy) {
    errno = 0;
    const int max = sched_get_priority_max(policy);
    const int max_errno = max == -1 ? errno : 0;
    errno = 0;
    const int min = sched_get_priority_min(policy);
    const int min_errno = min == -1 ? errno : 0;
    printf("policy %d:", policy);
    if (max_errno) printf(" max -1 errno %d (%s)", max_errno, strerror(max_errno));
    else printf(" max %d", max);
    if (min_errno) printf(", min -1 errno %d (%s)\n", min_errno, strerror(min_errno));
    else printf(", min %d\n", min);
}

int main(void) {
    for (int policy = -1; policy <= 7; ++policy) show(policy);
    show(99);
    show(0x40000000);  // SCHED_RESET_ON_FORK is a flag of sched_setscheduler, not a policy here
    printf("done\n");
    return 0;
}
