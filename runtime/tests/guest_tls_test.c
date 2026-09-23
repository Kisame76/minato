// Checks the Darwin TSD slot that holds the guest thread pointer (ADR-0019).
// The islands in converted guest code read the slot with a hard-coded offset, so the slot must be
// free in every thread and must never collide with a key from pthread_key_create.
#include "APKRuntime/apkrt.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) check((condition), #condition, __LINE__)

static void check(int condition, const char *expression, int line) {
    if (condition) return;
    fprintf(stderr, "guest_tls_test.c:%d: CHECK failed: %s\n", line, expression);
    ++failures;
}

// The islands hard-code this offset, so a change of the slot must break the build here as well.
_Static_assert(APKRT_GUEST_TLS_OFFSET == 48, "the island below loads from #48");

/// Reads the slot the way an island does: base from TPIDRRO_EL0, fixed offset, no call.
static void *read_slot_like_an_island(void) {
    void *value;
    __asm__ __volatile__(
        "mrs %0, TPIDRRO_EL0\n\t"
        "bic %0, %0, #7\n\t"
        "ldr %0, [%0, #48]"
        : "=&r"(value));
    return value;
}

/// Everything a thread has to prove about the slot. Returns the number of failures it saw.
static void *check_slot_on_this_thread(void *argument) {
    void *const wanted = argument;

    CHECK(read_slot_like_an_island() == NULL);  // free before the runtime uses it
    CHECK(apkrt_get_guest_tls() == NULL);

    apkrt_set_guest_tls(wanted);
    CHECK(apkrt_get_guest_tls() == wanted);
    CHECK(read_slot_like_an_island() == wanted);

    // libpthread must keep its hands off the slot: its own keys live elsewhere.
    pthread_key_t keys[64];
    for (size_t index = 0; index < sizeof keys / sizeof *keys; ++index) {
        CHECK(pthread_key_create(&keys[index], NULL) == 0);
        CHECK(keys[index] != APKRT_GUEST_TLS_SLOT);
        CHECK(pthread_setspecific(keys[index], (void *)(uintptr_t)(index + 1)) == 0);
    }
    CHECK(read_slot_like_an_island() == wanted);
    for (size_t index = 0; index < sizeof keys / sizeof *keys; ++index) {
        CHECK(pthread_getspecific(keys[index]) == (void *)(uintptr_t)(index + 1));
        CHECK(pthread_key_delete(keys[index]) == 0);
    }

    // Code of the host (malloc, stdio, locks) must not disturb the slot either.
    char *const scratch = malloc(4096);
    CHECK(scratch != NULL);
    memset(scratch, 0, 4096);
    (void)snprintf(scratch, 4096, "%p", wanted);
    free(scratch);
    CHECK(read_slot_like_an_island() == wanted);

    return NULL;
}

int main(void) {
    check_slot_on_this_thread((void *)0x1122334455667788ULL);

    // A fresh thread starts with an empty slot, so the runtime always sets it itself.
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, check_slot_on_this_thread, (void *)0x99aabbccddeeff00ULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);

    if (failures == 0) printf("ok   guest TLS in Darwin TSD slot %d\n", APKRT_GUEST_TLS_SLOT);
    return failures == 0 ? 0 : 1;
}
