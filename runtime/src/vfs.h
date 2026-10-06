// The guest's file system (ARCHITECTURE.md 4.5): a table of mounts that puts host directories and
// files under Android paths, and the guest's working directory. Guest paths outside every mount do
// not exist for the guest. The file syscalls (sys_files.cpp) resolve every path through here.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace apkrt::vfs {

/// The normal form of an absolute guest path, as the Linux kernel resolves it without symlinks: no
/// "." or ".." components, no repeated or trailing slashes. ".." of the root is the root.
std::string normalize(std::string_view absolute_path);

/// `path` in normal form; a relative path is taken relative to `base`, an absolute guest path.
std::string absolute(std::string_view path, std::string_view base);

/// Guest paths at and below `guest` are the host path `host`, a directory or a file. The deepest
/// mount covering a path wins; mounting the same guest path again replaces the earlier mount.
void mount(std::string_view guest, std::string_view host);

/// Back to the start: only the devices every Linux system has (/dev/null, /dev/zero, /dev/random,
/// /dev/urandom, backed by the host's), and the root as the working directory.
void reset();

/// The host path of a guest path in normal form, or nullopt when no mount covers it.
std::optional<std::string> host_path(std::string_view guest);

/// Where a guest path lies for a host call (ADR-0052): below `folder`, a descriptor of its mounted folder, which the
/// mount table keeps open; `path` is relative to it, "." for the folder itself. Host calls start at `folder` with
/// Darwin's RESOLVE_BENEATH, so that no link and no ".." leads out of the mount. A mounted file, such as a device, has
/// no folder: `folder` is AT_FDCWD and `path` its host path from the table, which host calls take as it is, since
/// iOS's sandbox lets an app open /dev/urandom but not read /dev. Nothing lies below a file: `below_a_file` then.
struct Location {
    int folder = -1;
    std::string path;
    bool below_a_file = false;
};

/// The Location of a guest path in normal form; nullopt when no mount covers it or its host folder is not there.
std::optional<Location> locate(std::string_view guest);

/// The guest path of a host path below a mount, the reverse of host_path, or nullopt.
std::optional<std::string> guest_path(std::string_view host);

/// The mount a guest path in normal form lies in, as Linux compares mounts for rename and link (EXDEV): the guest path
/// of the deepest mounted folder over it, where a mounted file or device lies in its folder's mount; nullopt when no
/// mounted folder covers it. A mount whose host path is not there counts as a folder.
std::optional<std::string> mount_point(std::string_view guest);

/// The guest's working directory, an absolute guest path in normal form; "/" at first.
std::string working_directory();
void set_working_directory(std::string_view guest);

}  // namespace apkrt::vfs
