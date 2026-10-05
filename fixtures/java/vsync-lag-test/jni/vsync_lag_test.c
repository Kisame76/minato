// vsync-lag-test's JNI library (ADR-0041 a) decision 6's fix): the frame clock's ticks, posted through graphics-run's
// test control, the front lib "test" (graphics/tests/test_control.h), whose table it binds as graphics/guest's
// minato_bind_host_table does and whose slot 1 it reaches through the runtime's entry in slot 0, as
// graphics/tests/test_control_guest.c's stub.
#include <jni.h>
#include <stdint.h>
#include <unistd.h>

#include "../../../../graphics/tests/test_control.h"
#include "../../../../runtime/include/APKRuntime/guest_interface.h"

__attribute__((visibility("hidden"))) void *minato_test_table[MINATO_TEST_TABLE_LENGTH];

__attribute__((naked)) uint64_t minato_test(uint64_t command, uint64_t a, uint64_t b, uint64_t c) {
    __asm__("adrp x17, minato_test_table\n\t"
            "add x17, x17, :lo12:minato_test_table\n\t"
            "ldr x16, [x17, #8]\n\t"
            "ldr x17, [x17]\n\t"
            "br x17");
}

// Whether graphics-run's provider filled the table; without it, no command may be called.
JNIEXPORT jboolean JNICALL Java_io_github_kisame76_minato_test_vsynclag_VsyncLagApplication_nativeBind(JNIEnv *env,
                                                                                                    jclass clazz) {
    (void)env;
    (void)clazz;
    return syscall(APKRT_NR_BIND_HOST_TABLE, "test", minato_test_table, MINATO_TEST_TABLE_LENGTH,
                   MINATO_TEST_TABLE_HASH) == 0;
}

// A tick at `time` with the period `period`, in nanoseconds of the guest's CLOCK_MONOTONIC (apkrt_vsync_post).
JNIEXPORT void JNICALL Java_io_github_kisame76_minato_test_vsynclag_VsyncLagApplication_nativeTick(JNIEnv *env,
                                                                                                 jclass clazz,
                                                                                                 jlong time,
                                                                                                 jlong period) {
    (void)env;
    (void)clazz;
    minato_test(MINATO_TEST_VSYNC_GRID, (uint64_t)time, (uint64_t)period, 0);
}
