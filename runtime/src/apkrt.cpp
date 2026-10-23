#include "APKRuntime/apkrt.h"

#include "dl.h"
#include "vfs.h"

#include <sys/param.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int fail(char *error, size_t error_size, const char *format, const char *path, const char *reason = "") {
    if (error != nullptr && error_size > 0) std::snprintf(error, error_size, format, path, reason);
    return -1;
}

}  // namespace

const char *apkrt_version(void) {
    return APKRT_VERSION;
}

int apkrt_mount(const char *guest_path, const char *host_path, char *error, size_t error_size) {
    if (guest_path == nullptr || guest_path[0] != '/') {
        return fail(error, error_size, "guest path \"%s\" is not absolute", guest_path ? guest_path : "");
    }
    // The canonical host path, so that host paths map back to guest paths (fchdir).
    char resolved[MAXPATHLEN];
    if (host_path == nullptr || ::realpath(host_path, resolved) == nullptr) {
        return fail(error, error_size, "host path \"%s\": %s", host_path ? host_path : "", std::strerror(errno));
    }
    apkrt::vfs::mount(guest_path, resolved);
    return 0;
}

int apkrt_set_working_directory(const char *guest_path, char *error, size_t error_size) {
    if (guest_path == nullptr || guest_path[0] != '/') {
        return fail(error, error_size, "working directory \"%s\" is not absolute", guest_path ? guest_path : "");
    }
    apkrt::vfs::set_working_directory(guest_path);
    return 0;
}

int apkrt_add_library_directory(const char *guest_directory, const char *host_path, char *error, size_t error_size) {
    if (guest_directory == nullptr || guest_directory[0] != '/') {
        return fail(error, error_size, "library directory \"%s\" is not absolute",
                    guest_directory ? guest_directory : "");
    }
    char resolved[MAXPATHLEN];
    if (host_path == nullptr || ::realpath(host_path, resolved) == nullptr) {
        return fail(error, error_size, "host path \"%s\": %s", host_path ? host_path : "", std::strerror(errno));
    }
    apkrt::dl::add_library_directory(guest_directory, resolved);
    return 0;
}

int apkrt_mount_live(const char *guest_path, const char *host_dir) {
    if (guest_path == nullptr || guest_path[0] != '/') return -EINVAL;
    if (std::strlen(guest_path) >= apkrt::vfs::path_max) return -ENAMETOOLONG;
    if (host_dir == nullptr || host_dir[0] != '/') return -EINVAL;  // an absolute host directory
    return apkrt::vfs::mount_live(guest_path, host_dir);
}

int apkrt_unmount(int id) {
    return apkrt::vfs::unmount(id);
}
