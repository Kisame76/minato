// libartpalette-system.so for the guest (ADR-0033 a): the system half of ART's palette, which libartpalette opens
// at start and aborts without. After AOSP's palette_fake.cc: a thread's Java priority is kept in a table, as the
// guest has no scheduler priorities to set (ADR-0033 decision 8); there is no tracing, no ashmem and nobody to
// notify of compilations or JNI calls. Crash stacks go to the log.
#include <android/log.h>
#include <stdbool.h>

#include <map>
#include <mutex>

#include "palette/palette.h"

namespace {

// java.lang.Thread's MIN_PRIORITY, NORM_PRIORITY and MAX_PRIORITY.
constexpr int32_t kMinPriority = 1;
constexpr int32_t kNormalPriority = 5;
constexpr int32_t kMaxPriority = 10;

std::mutex priorities_lock;
std::map<int32_t, int32_t> priorities;

}  // namespace

palette_status_t PaletteSchedSetPriority(int32_t tid, int32_t java_priority) {
    if (java_priority < kMinPriority || java_priority > kMaxPriority) return PALETTE_STATUS_INVALID_ARGUMENT;
    const std::lock_guard<std::mutex> hold(priorities_lock);
    priorities[tid] = java_priority;
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteSchedGetPriority(int32_t tid, int32_t* java_priority) {
    const std::lock_guard<std::mutex> hold(priorities_lock);
    const auto found = priorities.try_emplace(tid, kNormalPriority).first;
    *java_priority = found->second;
    return PALETTE_STATUS_OK;
}

palette_status_t PaletteWriteCrashThreadStacks(const char* stacks, size_t stacks_len) {
    __android_log_print(ANDROID_LOG_INFO, "libartpalette-system", "%.*s", static_cast<int>(stacks_len), stacks);
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
