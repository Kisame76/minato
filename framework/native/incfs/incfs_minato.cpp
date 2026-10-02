// The part of libincfs (system/incremental_delivery/incfs) that libincfs-utils' map_ptr.cpp calls on Android
// (ADR-0039 b3.2): whether a descriptor is on the incremental file system, and whether such a file is fully
// loaded. Minato installs no app on incfs, and AOSP's libincfs would bring SELinux, sysprop and BoringSSL with it.
// IncFs_IsIncFsFd answers as AOSP's isIncFsFdImpl does, from fstatfs's file system type, so no descriptor is on
// incfs and map_ptr never checks a block's presence.
#include <errno.h>
#include <limits.h>
#include <sys/vfs.h>

#include <incfs_ndk.h>
#include <linux/incrementalfs.h>

bool IncFs_IsIncFsFd(int fd) {
    struct statfs fs = {};
    if (fstatfs(fd, &fs) != 0) return false;
    return fs.f_type == static_cast<decltype(fs.f_type)>(INCFS_MAGIC_NUMBER);
}

// Only asked for a descriptor on incfs (map_ptr.cpp's IsVerificationEnabled), which has none here.
IncFsErrorCode IncFs_IsFullyLoaded(int) {
    return -ENOTSUP;
}
