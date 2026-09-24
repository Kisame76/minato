// Checks the Darwin TSD slots that hold the guest thread pointer (slot 6, ADR-0019) and the guest's
// x18 (slot 11, ADR-0020). The islands in converted guest code read them with a hard-coded offset,
// so both must be free in every thread and must never collide with a key from pthread_key_create
// or with the slots in which Libc keeps its own per-thread state.
#include "APKRuntime/apkrt.h"

#include <locale.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition) check((condition), #condition, __LINE__)

static void check(int condition, const char *expression, int line) {
    if (condition) return;
    fprintf(stderr, "guest_tls_test.c:%d: CHECK failed: %s\n", line, expression);
    ++failures;
}

// The islands hard-code these offsets, so a change of a slot must break the build here as well.
_Static_assert(APKRT_GUEST_TLS_OFFSET == 48, "the island below loads from #48");
_Static_assert(APKRT_GUEST_X18_OFFSET == 88, "the island below loads from #88");

/// Reads slot 6 the way an island does: base from TPIDRRO_EL0, fixed offset, no call.
static void *read_slot_6_like_an_island(void) {
    void *value;
    __asm__ __volatile__(
        "mrs %0, TPIDRRO_EL0\n\t"
        "bic %0, %0, #7\n\t"
        "ldr %0, [%0, #48]"
        : "=&r"(value));
    return value;
}

/// Reads slot 11 the way an island does.
static uint64_t read_slot_11_like_an_island(void) {
    uint64_t value;
    __asm__ __volatile__(
        "mrs %0, TPIDRRO_EL0\n\t"
        "bic %0, %0, #7\n\t"
        "ldr %0, [%0, #88]"
        : "=&r"(value));
    return value;
}

/// What host code does with the TSD: pthread keys, malloc and stdio, and the Libc functions that keep
/// per-thread state in slots of their own next to slot 11 (ttyname, localtime, gmtime, strtod,
/// setlocale). No key may be one of the runtime's slots.
static void use_the_tsd_like_host_code(const void *text) {
    pthread_key_t keys[64];
    for (size_t index = 0; index < sizeof keys / sizeof *keys; ++index) {
        CHECK(pthread_key_create(&keys[index], NULL) == 0);
        CHECK(keys[index] != APKRT_GUEST_TLS_SLOT && keys[index] != APKRT_GUEST_X18_SLOT);
        CHECK(pthread_setspecific(keys[index], (void *)(uintptr_t)(index + 1)) == 0);
    }
    for (size_t index = 0; index < sizeof keys / sizeof *keys; ++index) {
        CHECK(pthread_getspecific(keys[index]) == (void *)(uintptr_t)(index + 1));
        CHECK(pthread_key_delete(keys[index]) == 0);
    }

    char *const scratch = malloc(4096);
    CHECK(scratch != NULL);
    memset(scratch, 0, 4096);
    (void)snprintf(scratch, 4096, "%p", text);
    free(scratch);

    (void)ttyname(STDIN_FILENO);  // off the main thread it takes its buffer from the TSD first
    const time_t now = time(NULL);
    CHECK(localtime(&now) != NULL);
    CHECK(gmtime(&now) != NULL);
    CHECK(strtod("1.5e300", NULL) == 1.5e300);  // gdtoa's big integers
    CHECK(setlocale(LC_ALL, "") != NULL);
    CHECK(setlocale(LC_ALL, "C") != NULL);
}

/// Everything a thread has to prove about the two slots.
static void *check_slots_on_this_thread(void *argument) {
    void *const thread_pointer = argument;
    const uint64_t x18 = (uint64_t)(uintptr_t)argument ^ 0x1818181818181818ULL;

    // Free before the runtime uses them.
    CHECK(read_slot_6_like_an_island() == NULL);
    CHECK(apkrt_get_guest_tls() == NULL);
    CHECK(read_slot_11_like_an_island() == 0);
    CHECK(apkrt_get_guest_x18() == 0);

    apkrt_set_guest_tls(thread_pointer);
    apkrt_set_guest_x18(x18);
    CHECK(apkrt_get_guest_tls() == thread_pointer);
    CHECK(read_slot_6_like_an_island() == thread_pointer);
    CHECK(apkrt_get_guest_x18() == x18);
    CHECK(read_slot_11_like_an_island() == x18);

    // Host code keeps its hands off both.
    use_the_tsd_like_host_code(thread_pointer);
    CHECK(read_slot_6_like_an_island() == thread_pointer);
    CHECK(read_slot_11_like_an_island() == x18);
    return NULL;
}

int main(void) {
    check_slots_on_this_thread((void *)0x1122334455667788ULL);

    // A fresh thread starts with empty slots, so the runtime always sets them itself.
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, check_slots_on_this_thread, (void *)0x99aabbccddeeff00ULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);

    if (failures == 0) {
        printf("ok   guest TLS in Darwin TSD slot %d, guest x18 in slot %d\n", APKRT_GUEST_TLS_SLOT, APKRT_GUEST_X18_SLOT);
    }
    return failures == 0 ? 0 : 1;
}
