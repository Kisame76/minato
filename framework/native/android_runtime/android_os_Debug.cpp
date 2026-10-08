// Ours in place of AOSP's core/jni/android_os_Debug.cpp (the framework breadth's batch 3b): android.os.Debug's 30
// natives, so that an app or an SDK that asks for its memory use (getNativeHeapAllocatedSize, getMemoryInfo, getPss)
// gets an answer and not an UnsatisfiedLinkError. The table and the signatures are the tag's [verified: frameworks/base
// at android-15.0.0_r1]. This is guest code on Bionic, so the numbers come from what AOSP reads where the runtime has
// it: mallinfo for the native heap, /proc/self/stat for the resident size (libmeminfo's StatusVmRSS reads the same),
// /proc/meminfo for the system's memory (libmeminfo's SysMemInfo). Not from AOSP:
//   - Pss and Uss are the resident size, since nothing here is shared that the process can tell; the runtime's
//     /proc/self/smaps, which AOSP sums, takes tens of seconds, too long for an app that polls its memory;
//   - the Java heap is Runtime's used memory, and "other" is the Pss beyond the native and Java heaps;
//   - only the app's own process can be asked, as on Android, where another app's smaps are unreadable: any other pid
//     gets false or 0;
//   - what a device without the data answers, as the tag's own failure values: -1 for the binder driver's
//     statistics and for the ion, dma-buf and GPU counters, 0 for zram, the binder object counts and dma-buf
//     mappings, false for vmap stacks, backtraces and the allocator's log, and an empty dump for the native heap.
#include <jni.h>
#include <malloc.h>
#include <nativehelper/JNIHelp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>

#include "core_jni_helpers.h"

namespace android {
namespace {

// getMemInfo's array, in the order of Debug.java's MEMINFO_ constants (the tag's default tags with Zram inserted), and
// its length.
constexpr int kMeminfoCount = 26;
const char *const kMeminfoTags[kMeminfoCount] = {
        "MemTotal:", "MemFree:", "Buffers:", "Cached:", "Shmem:", "Slab:", "SReclaimable:", "SUnreclaim:",
        "SwapTotal:", "SwapFree:", "Zram:", "Mapped:", "VmallocUsed:", "PageTables:", "KernelStack:",
        "KReclaimable:", "Active:", "Inactive:", "Unevictable:", "MemAvailable:", "Active(anon):",
        "Inactive(anon):", "Active(file):", "Inactive(file):", "CmaTotal:", "CmaFree:"};

// The tag's three core heaps, in the order of its stat_field_names: other, dalvik, native.
constexpr int kCoreHeaps = 3;
const char *const kFieldNames[kCoreHeaps][9] = {
        {"otherPss", "otherSwappablePss", "otherRss", "otherPrivateDirty", "otherSharedDirty", "otherPrivateClean",
         "otherSharedClean", "otherSwappedOut", "otherSwappedOutPss"},
        {"dalvikPss", "dalvikSwappablePss", "dalvikRss", "dalvikPrivateDirty", "dalvikSharedDirty",
         "dalvikPrivateClean", "dalvikSharedClean", "dalvikSwappedOut", "dalvikSwappedOutPss"},
        {"nativePss", "nativeSwappablePss", "nativeRss", "nativePrivateDirty", "nativeSharedDirty",
         "nativePrivateClean", "nativeSharedClean", "nativeSwappedOut", "nativeSwappedOutPss"}};
jfieldID gHeapFields[kCoreHeaps][9];
jfieldID gHasSwappedOutPss;

// The process's memory in kB. The resident size is field 24 of /proc/self/stat; nothing of the process is shared with
// another one that this runtime lets it tell, so Pss and Uss are the resident size too and its pages count as private
// dirty. (The runtime's /proc/self/smaps is exact, but it takes tens of seconds, which an app that polls its memory
// cannot wait for.) 0 where the file is not there.
struct Footprint {
    uint64_t pss = 0;
    uint64_t rss = 0;
    uint64_t privateDirty = 0;
    uint64_t privateClean = 0;
};

Footprint ProcessFootprint() {
    Footprint result;
    FILE *stat = fopen("/proc/self/stat", "re");
    if (stat == nullptr) {
        return result;
    }
    char line[1024];
    const char *end = fgets(line, sizeof(line), stat) != nullptr ? strrchr(line, ')') : nullptr;
    fclose(stat);
    long pages = 0;
    // After the command: the state, then fields 4 to 22, the virtual size and the resident pages.
    if (end != nullptr &&
        sscanf(end + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %ld",
               &pages) == 1 && pages > 0) {
        result.rss = static_cast<uint64_t>(pages) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) / 1024;
        result.pss = result.rss;
        result.privateDirty = result.rss;
    }
    return result;
}

jlong NativeHeapSize(JNIEnv *, jobject) {
    return static_cast<jlong>(mallinfo().usmblks);
}

jlong NativeHeapAllocatedSize(JNIEnv *, jobject) {
    return static_cast<jlong>(mallinfo().uordblks);
}

jlong NativeHeapFreeSize(JNIEnv *, jobject) {
    return static_cast<jlong>(mallinfo().fordblks);
}

// Runtime's getRuntime, totalMemory and freeMemory, resolved once at registration.
jclass gRuntimeClass;
jmethodID gGetRuntime;
jmethodID gTotalMemory;
jmethodID gFreeMemory;

// The Java heap in use, in bytes: Runtime's total memory less its free memory; 0 where Runtime does not answer.
uint64_t JavaHeapUsed(JNIEnv *env) {
    jobject instance = env->CallStaticObjectMethod(gRuntimeClass, gGetRuntime);
    if (instance == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    jlong used = env->CallLongMethod(instance, gTotalMemory) - env->CallLongMethod(instance, gFreeMemory);
    env->DeleteLocalRef(instance);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
    return used > 0 ? static_cast<uint64_t>(used) : 0;
}

jboolean GetMemoryInfoPid(JNIEnv *env, jobject, jint pid, jobject info) {
    if (pid != getpid()) {
        return JNI_FALSE;
    }
    Footprint process = ProcessFootprint();
    int native = static_cast<int>(mallinfo().uordblks / 1024);
    int dalvik = static_cast<int>(JavaHeapUsed(env) / 1024);
    int other = std::max<int>(0, static_cast<int>(process.pss) - native - dalvik);
    const int pss[kCoreHeaps] = {other, dalvik, native};
    for (int heap = 0; heap < kCoreHeaps; heap++) {
        // Pss, Rss and the private dirty pages are the heap's own; nothing here is shared, clean or swapped.
        for (int field = 0; field < 9; field++) {
            bool own = field == 0 || field == 2 || field == 3;
            env->SetIntField(info, gHeapFields[heap][field], own ? pss[heap] : 0);
        }
    }
    // MemoryInfo's otherStats, which the tag fills per further heap (graphics, code, stack), stay 0.
    env->SetBooleanField(info, gHasSwappedOutPss, JNI_FALSE);
    return JNI_TRUE;
}

void GetMemoryInfo(JNIEnv *env, jobject clazz, jobject info) {
    GetMemoryInfoPid(env, clazz, getpid(), info);
}

jlong GetPssPid(JNIEnv *env, jobject, jint pid, jlongArray outUssSwapPssRss, jlongArray outMemtrack) {
    if (pid != getpid()) {
        return 0;
    }
    Footprint process = ProcessFootprint();
    jlong pss = static_cast<jlong>(process.pss);
    if (outUssSwapPssRss != nullptr) {
        jsize length = env->GetArrayLength(outUssSwapPssRss);
        jlong values[3] = {static_cast<jlong>(process.privateDirty + process.privateClean), 0,
                           static_cast<jlong>(process.rss)};
        env->SetLongArrayRegion(outUssSwapPssRss, 0, std::min<jsize>(length, 3), values);
    }
    if (outMemtrack != nullptr) {
        jsize length = env->GetArrayLength(outMemtrack);
        jlong values[4] = {0, 0, 0, 0};
        env->SetLongArrayRegion(outMemtrack, 0, std::min<jsize>(length, 4), values);
    }
    return pss;
}

jlong GetPss(JNIEnv *env, jobject clazz) {
    return GetPssPid(env, clazz, getpid(), nullptr, nullptr);
}

jlong GetRssPid(JNIEnv *env, jobject, jint pid, jlongArray outMemtrack) {
    if (pid != getpid()) {
        return 0;
    }
    if (outMemtrack != nullptr) {
        jsize length = env->GetArrayLength(outMemtrack);
        jlong values[4] = {0, 0, 0, 0};
        env->SetLongArrayRegion(outMemtrack, 0, std::min<jsize>(length, 4), values);
    }
    return static_cast<jlong>(ProcessFootprint().rss);
}

jlong GetRss(JNIEnv *env, jobject clazz) {
    return GetRssPid(env, clazz, getpid(), nullptr);
}

void GetMemInfo(JNIEnv *env, jobject, jlongArray out) {
    if (out == nullptr) {
        jniThrowNullPointerException(env, "out == null");
        return;
    }
    if (env->GetArrayLength(out) < kMeminfoCount) {
        jniThrowRuntimeException(env, "outLen < MEMINFO_COUNT");
        return;
    }
    jlong values[kMeminfoCount] = {};
    FILE *meminfo = fopen("/proc/meminfo", "re");
    if (meminfo == nullptr) {
        jniThrowRuntimeException(env, "SysMemInfo read failed");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), meminfo) != nullptr) {
        for (int tag = 0; tag < kMeminfoCount; tag++) {
            size_t length = strlen(kMeminfoTags[tag]);
            unsigned long long value;
            if (strncmp(line, kMeminfoTags[tag], length) == 0 && sscanf(line + length, " %llu", &value) == 1) {
                values[tag] = static_cast<jlong>(value);
            }
        }
    }
    fclose(meminfo);
    env->SetLongArrayRegion(out, 0, kMeminfoCount, values);
}

void DumpNativeHeap(JNIEnv *, jobject, jobject) {}
void DumpNativeMallocInfo(JNIEnv *, jobject, jobject) {}

jint MinusOneInt(JNIEnv *, jobject) {
    return -1;
}

jint ZeroInt(JNIEnv *, jobject) {
    return 0;
}

jboolean DumpBacktrace(JNIEnv *, jobject, jint, jstring, jint) {
    return JNI_FALSE;
}

jstring GetUnreachableMemory(JNIEnv *env, jobject, jint, jboolean) {
    return env->NewStringUTF("Failed to get unreachable memory");
}

jlong ZeroLong(JNIEnv *, jobject) {
    return 0;
}

jlong MinusOneLong(JNIEnv *, jobject) {
    return -1;
}

jboolean False(JNIEnv *, jobject) {
    return JNI_FALSE;
}

const JNINativeMethod gMethods[] = {
        {"getNativeHeapSize", "()J", reinterpret_cast<void *>(NativeHeapSize)},
        {"getNativeHeapAllocatedSize", "()J", reinterpret_cast<void *>(NativeHeapAllocatedSize)},
        {"getNativeHeapFreeSize", "()J", reinterpret_cast<void *>(NativeHeapFreeSize)},
        {"getMemoryInfo", "(Landroid/os/Debug$MemoryInfo;)V", reinterpret_cast<void *>(GetMemoryInfo)},
        {"getMemoryInfo", "(ILandroid/os/Debug$MemoryInfo;)Z", reinterpret_cast<void *>(GetMemoryInfoPid)},
        {"getPss", "()J", reinterpret_cast<void *>(GetPss)},
        {"getPss", "(I[J[J)J", reinterpret_cast<void *>(GetPssPid)},
        {"getRss", "()J", reinterpret_cast<void *>(GetRss)},
        {"getRss", "(I[J)J", reinterpret_cast<void *>(GetRssPid)},
        {"getMemInfo", "([J)V", reinterpret_cast<void *>(GetMemInfo)},
        {"dumpNativeHeap", "(Ljava/io/FileDescriptor;)V", reinterpret_cast<void *>(DumpNativeHeap)},
        {"dumpNativeMallocInfo", "(Ljava/io/FileDescriptor;)V", reinterpret_cast<void *>(DumpNativeMallocInfo)},
        {"getBinderSentTransactions", "()I", reinterpret_cast<void *>(MinusOneInt)},
        {"getBinderReceivedTransactions", "()I", reinterpret_cast<void *>(MinusOneInt)},
        {"getBinderLocalObjectCount", "()I", reinterpret_cast<void *>(ZeroInt)},
        {"getBinderProxyObjectCount", "()I", reinterpret_cast<void *>(ZeroInt)},
        {"getBinderDeathObjectCount", "()I", reinterpret_cast<void *>(ZeroInt)},
        {"dumpJavaBacktraceToFileTimeout", "(ILjava/lang/String;I)Z", reinterpret_cast<void *>(DumpBacktrace)},
        {"dumpNativeBacktraceToFileTimeout", "(ILjava/lang/String;I)Z", reinterpret_cast<void *>(DumpBacktrace)},
        {"getUnreachableMemory", "(IZ)Ljava/lang/String;", reinterpret_cast<void *>(GetUnreachableMemory)},
        {"getZramFreeKb", "()J", reinterpret_cast<void *>(ZeroLong)},
        {"getIonHeapsSizeKb", "()J", reinterpret_cast<void *>(MinusOneLong)},
        {"getDmabufTotalExportedKb", "()J", reinterpret_cast<void *>(MinusOneLong)},
        {"getGpuPrivateMemoryKb", "()J", reinterpret_cast<void *>(MinusOneLong)},
        {"getDmabufHeapTotalExportedKb", "()J", reinterpret_cast<void *>(MinusOneLong)},
        {"getIonPoolsSizeKb", "()J", reinterpret_cast<void *>(MinusOneLong)},
        {"getDmabufMappedSizeKb", "()J", reinterpret_cast<void *>(ZeroLong)},
        {"getDmabufHeapPoolsSizeKb", "()J", reinterpret_cast<void *>(MinusOneLong)},
        {"getGpuTotalUsageKb", "()J", reinterpret_cast<void *>(MinusOneLong)},
        {"isVmapStack", "()Z", reinterpret_cast<void *>(False)},
        {"logAllocatorStats", "()Z", reinterpret_cast<void *>(False)},
};

}  // namespace

int register_android_os_Debug(JNIEnv *env) {
    jclass memoryInfo = FindClassOrDie(env, "android/os/Debug$MemoryInfo");
    for (int heap = 0; heap < kCoreHeaps; heap++) {
        for (int field = 0; field < 9; field++) {
            gHeapFields[heap][field] = GetFieldIDOrDie(env, memoryInfo, kFieldNames[heap][field], "I");
        }
    }
    gHasSwappedOutPss = GetFieldIDOrDie(env, memoryInfo, "hasSwappedOutPss", "Z");
    jclass runtime = FindClassOrDie(env, "java/lang/Runtime");
    gRuntimeClass = MakeGlobalRefOrDie(env, runtime);
    gGetRuntime = GetStaticMethodIDOrDie(env, runtime, "getRuntime", "()Ljava/lang/Runtime;");
    gTotalMemory = GetMethodIDOrDie(env, runtime, "totalMemory", "()J");
    gFreeMemory = GetMethodIDOrDie(env, runtime, "freeMemory", "()J");
    return RegisterMethodsOrDie(env, "android/os/Debug", gMethods, NELEM(gMethods));
}

}  // namespace android
