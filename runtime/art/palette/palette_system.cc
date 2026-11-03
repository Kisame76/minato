// libartpalette-system.so for the guest (ADR-0033 a): the system half of ART's palette, which libartpalette opens
// at start and aborts without. After AOSP's palette_fake.cc, but for thread priorities: a Java thread's priority
// sets its nice value, as Android 15's system palette does at Thread.setPriority and at each Java thread's start
// (the Unity test APK probe's blocker 7), over the runtime's nice value per thread (ADR-0033 decision 8, changed
// 2026-10-08), which iOS's scheduling does not see. There is no tracing, no ashmem and nobody to notify of
// compilations or JNI calls. Crash stacks go to the log, line by line.
#include <android/log.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/resource.h>

#include "palette/palette.h"

namespace {

// java.lang.Thread's MIN_PRIORITY, NORM_PRIORITY and MAX_PRIORITY.
constexpr int32_t kMinPriority = 1;
constexpr int32_t kNormalPriority = 5;
constexpr int32_t kMaxPriority = 10;

// The nice value of each Java priority from MIN_PRIORITY up, Android 15's: the lowest, background + 6, + 3, the
// background's, the default, -2, -4, urgent display + 3, + 2, and urgent display's
// [verified: system/libartpalette's palette_android.cc at android-15.0.0_r1].
constexpr int kNiceValues[] = {19, 16, 13, 10, 0, -2, -4, -5, -6, -8};

}  // namespace

// The priority's nice value for the thread, unless it has it already, as Android 15's; neither sets task profiles.
palette_status_t PaletteSchedSetPriority(int32_t tid, int32_t java_priority) {
    if (java_priority < kMinPriority || java_priority > kMaxPriority) return PALETTE_STATUS_INVALID_ARGUMENT;
    const int nice = kNiceValues[java_priority - kMinPriority];
    if (getpriority(PRIO_PROCESS, tid) == nice) return PALETTE_STATUS_OK;
    return setpriority(PRIO_PROCESS, tid, nice) == 0 ? PALETTE_STATUS_OK : PALETTE_STATUS_CHECK_ERRNO;
}

// The Java priority of the thread's nice value: the lowest whose nice value the thread's reaches, MAX_PRIORITY above
// them all, as Android 15's; NORM_PRIORITY when the thread is not there.
palette_status_t PaletteSchedGetPriority(int32_t tid, int32_t* java_priority) {
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, tid);
    if (nice == -1 && errno != 0) {
        *java_priority = kNormalPriority;
        return PALETTE_STATUS_CHECK_ERRNO;
    }
    for (int32_t priority = kMinPriority; priority <= kMaxPriority; priority++) {
        if (nice >= kNiceValues[priority - kMinPriority]) {
            *java_priority = priority;
            return PALETTE_STATUS_OK;
        }
    }
    *java_priority = kMaxPriority;
    return PALETTE_STATUS_OK;
}

// ART's SIGQUIT dump (SignalCatcher::Output), which Android's palette hands to tombstoned for /data/anr: a message
// per line, since liblog cuts a message at its 4 KB payload and a dump of every thread runs far longer.
palette_status_t PaletteWriteCrashThreadStacks(const char* stacks, size_t stacks_len) {
    const char* end = stacks + stacks_len;
    for (const char* line = stacks; line < end;) {
        const char* next = static_cast<const char*>(memchr(line, '\n', static_cast<size_t>(end - line)));
        if (next == nullptr) next = end;
        __android_log_print(ANDROID_LOG_INFO, "libartpalette-system", "%.*s", static_cast<int>(next - line), line);
        line = next + 1;
    }
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteTraceEnabled(bool* enabled) {
    *enabled = false;
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteTraceBegin(const char*) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteTraceEnd() {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteTraceIntegerValue(const char*, int32_t) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteAshmemCreateRegion(const char*, size_t, int*) {
    return PALETTE_STATUS_NOT_SUPPORTED;
}

palette_status_t PaletteAshmemSetProtRegion(int, int) {
    return PALETTE_STATUS_NOT_SUPPORTED;
}

palette_status_t PaletteCreateOdrefreshStagingDirectory(const char** staging_dir) {
    *staging_dir = nullptr;
    return PALETTE_STATUS_NOT_SUPPORTED;
}

palette_status_t PaletteShouldReportDex2oatCompilation(bool* value) {
    *value = false;
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteNotifyStartDex2oatCompilation(int, int, int, int) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteNotifyEndDex2oatCompilation(int, int, int, int) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteNotifyDexFileLoaded(const char*) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteNotifyOatFileLoaded(const char*) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteShouldReportJniInvocations(bool* value) {
    *value = false;
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteNotifyBeginJniInvocation(JNIEnv*) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteNotifyEndJniInvocation(JNIEnv*) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteReportLockContention(JNIEnv*, int32_t, const char*, int32_t, const char*, const char*, int32_t,
                                             const char*, const char*, const char*) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteSetTaskProfiles(int32_t, const char* const[], size_t) {
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteDebugStoreGetString(char* result, size_t max_size) {
    if (max_size > 0) result[0] = '\0';
    return PALETTE_STATUS_OK;
}
