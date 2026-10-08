/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Changed by Minato from AOSP's native/android/sharedmem.cpp (2017) and the memfd path of libcutils/ashmem-dev.cpp
// (2008) at android-15.0.0_r1, merged into this file.
// libandroid.so's ASharedMemory (API 26 and 27), as Android 15's native/android/sharedmem.cpp has it on libcutils'
// ashmem, with libcutils' memfd path written out here (ashmem-dev.cpp), the one that Minato's properties choose
// (sys.use_memfd and ro.treble.enabled): a region is a memfd whose size its seals keep, F_SEAL_GROW and F_SEAL_SHRINK,
// and setProt to read-only seals it against writes from then on, F_SEAL_FUTURE_WRITE with F_SEAL_SEAL. libcutils is
// no library of the guest's system, only framework-native's static one, so these lines stand in for its memfd
// functions. A descriptor that is no memfd has size 0, as a device whose regions are ashmem's answers.
#include <android/log.h>
#include <android/sharedmem.h>
#include <android/sharedmem_jni.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {

int ASharedMemory_create(const char* name, size_t size) {
    if (size == 0) return -EINVAL;  // android::BAD_VALUE
    // memfd_create_region: the memfd, its size, and the seals that keep that size, as ashmem's is fixed.
    const char* const shown = name != nullptr ? name : "none";
    const int fd = static_cast<int>(syscall(__NR_memfd_create, shown, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd == -1) {
        __android_log_print(ANDROID_LOG_ERROR, "ashmem", "memfd_create(%s, %zu) failed: %s\n", shown, size,
                            strerror(errno));
        return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) == -1 || fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) == -1) {
        const int error = errno;
        __android_log_print(ANDROID_LOG_ERROR, "ashmem", "memfd_create(%s, %zu) failed to size and seal: %s\n",
                            shown, size, strerror(error));
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

size_t ASharedMemory_getSize(int fd) {
    struct stat status;
    if (fcntl(fd, F_GET_SEALS) == -1 || fstat(fd, &status) == -1) return 0;
    return static_cast<size_t>(status.st_size);
}

int ASharedMemory_setProt(int fd, int prot) {
    // memfd_set_prot_region: writable stays only what never was sealed read-only; read-only seals future writes.
    const int seals = fcntl(fd, F_GET_SEALS);
    if (seals == -1) {
        __android_log_print(ANDROID_LOG_ERROR, "ashmem", "memfd_set_prot_region(%d, %d): F_GET_SEALS failed: %s\n",
                            fd, prot, strerror(errno));
        return -1;
    }
    if (prot & PROT_WRITE) {
        // A widening answers EINVAL without a log line, as Android 15's ashmem does (libcutils' memfd path logs
        // one).
        if (seals & F_SEAL_FUTURE_WRITE) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) == -1) {
        __android_log_print(ANDROID_LOG_ERROR, "ashmem",
                            "memfd_set_prot_region(%d, %d): F_SEAL_FUTURE_WRITE | F_SEAL_SEAL seal failed: %s\n", fd,
                            prot, strerror(errno));
        return -1;
    }
    return 0;
}

int ASharedMemory_dupFromJava(JNIEnv* env, jobject java_shared_memory) {
    if (env == nullptr || java_shared_memory == nullptr) return -1;
    // sharedmem.cpp's jniInit keeps SharedMemory's class and getFd in globals, made once under a lock; this library is
    // built without C++'s own, so each call looks them up instead, with the same fatal answers.
    jclass clazz = env->FindClass("android/os/SharedMemory");
    if (clazz == nullptr) __android_log_assert(nullptr, nullptr, "Failed to find android.os.SharedMemory");
    const jmethodID get_fd = env->GetMethodID(clazz, "getFd", "()I");
    if (get_fd == nullptr) __android_log_assert(nullptr, nullptr, "Failed to find method SharedMemory#getFd()");
    const bool instance = env->IsInstanceOf(java_shared_memory, clazz);
    env->DeleteLocalRef(clazz);
    if (!instance) {
        __android_log_print(ANDROID_LOG_WARN, nullptr,
                            "ASharedMemory_dupFromJava called with object that's not an instanceof "
                            "android.os.SharedMemory");
        return -1;
    }
    int fd = env->CallIntMethod(java_shared_memory, get_fd);
    if (fd != -1) fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    return fd;
}

}  // extern "C"
