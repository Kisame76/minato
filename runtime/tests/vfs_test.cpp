// Checks the path table of the guest's file system (ARCHITECTURE.md 4.5): Android paths, their
// normal form, and which host path a guest path is.
#include "vfs.h"

#include "check.h"

#include <string>

namespace {

using apkrt::vfs::absolute;
using apkrt::vfs::guest_path;
using apkrt::vfs::host_path;
using apkrt::vfs::normalize;

TEST(normalize_resolves_dots_and_repeated_slashes) {
    CHECK(normalize("/") == "/");
    CHECK(normalize("//") == "/");
    CHECK(normalize("/data//local/./tmp/") == "/data/local/tmp");
    CHECK(normalize("/data/local/../app") == "/data/app");
    CHECK(normalize("/data/..") == "/");
    CHECK(normalize("/..") == "/");  // Linux: ".." of the root is the root
    CHECK(normalize("/../data/../../x") == "/x");
    CHECK(normalize("/a/b/c/../../d") == "/a/d");
    CHECK(normalize("/...") == "/...");  // three dots are a name
    CHECK(normalize("/.hidden/..x") == "/.hidden/..x");
}

TEST(absolute_resolves_relative_paths_against_a_base) {
    CHECK(absolute("file", "/data/local/tmp") == "/data/local/tmp/file");
    CHECK(absolute("./a/../b", "/data") == "/data/b");
    CHECK(absolute("..", "/data/local") == "/data");
    CHECK(absolute("/etc/hosts", "/data") == "/etc/hosts");
    CHECK(absolute("", "/data") == "/data");
    CHECK(absolute("x", "/") == "/x");
}

TEST(a_mount_covers_its_path_and_everything_below) {
    apkrt::vfs::reset();
    apkrt::vfs::mount("/data/local/tmp", "/host/tmp");
    CHECK(host_path("/data/local/tmp") == "/host/tmp");
    CHECK(host_path("/data/local/tmp/a/b") == "/host/tmp/a/b");
    CHECK(!host_path("/data/local/tmpfile"));  // only at a path component boundary
    CHECK(!host_path("/data/local"));
    CHECK(!host_path("/"));
    CHECK(!host_path("/system/lib64/libc.so"));
}

TEST(the_longest_mount_wins) {
    apkrt::vfs::reset();
    apkrt::vfs::mount("/data", "/host/data");
    apkrt::vfs::mount("/data/local/tmp", "/host/tmp");
    CHECK(host_path("/data/app") == "/host/data/app");
    CHECK(host_path("/data/local") == "/host/data/local");
    CHECK(host_path("/data/local/tmp/x") == "/host/tmp/x");
}

TEST(a_later_mount_of_the_same_path_replaces_the_earlier) {
    apkrt::vfs::reset();
    apkrt::vfs::mount("/dev/random", "/dev/random");
    apkrt::vfs::mount("/dev/random", "/dev/urandom");
    CHECK(host_path("/dev/random") == "/dev/urandom");
}

TEST(a_mount_of_the_root_covers_every_path) {
    apkrt::vfs::reset();
    apkrt::vfs::mount("/", "/host/root");
    CHECK(host_path("/") == "/host/root");
    CHECK(host_path("/etc/hosts") == "/host/root/etc/hosts");
}

TEST(host_paths_map_back_to_guest_paths) {
    apkrt::vfs::reset();
    apkrt::vfs::mount("/data", "/host/data");
    apkrt::vfs::mount("/data/local/tmp", "/host/tmp");
    CHECK(guest_path("/host/tmp") == "/data/local/tmp");
    CHECK(guest_path("/host/tmp/a/b") == "/data/local/tmp/a/b");
    CHECK(guest_path("/host/data/app") == "/data/app");
    CHECK(!guest_path("/host/tmpfile"));
    CHECK(!guest_path("/Users"));
}

TEST(the_working_directory_starts_at_the_root) {
    apkrt::vfs::reset();
    CHECK(apkrt::vfs::working_directory() == "/");
    apkrt::vfs::set_working_directory("/data/local/tmp");
    CHECK(apkrt::vfs::working_directory() == "/data/local/tmp");
    apkrt::vfs::reset();
    CHECK(apkrt::vfs::working_directory() == "/");
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
