// Checks the path table of the guest's file system (ARCHITECTURE.md 4.5): Android paths, their
// normal form, and which host path a guest path is.
#include "vfs.h"

#include "check.h"

#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
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

TEST(a_paths_mount_point_is_the_deepest_mounted_folder_over_it) {
    apkrt::vfs::reset();
    apkrt::vfs::mount("/data", "/host/data");
    apkrt::vfs::mount("/data/user/0/io.example", "/host/app-data");
    apkrt::vfs::mount("/data/app/~~a/io.example-b/base.apk", "/dev/null");  // a mounted file, a device here
    // The mount lies at /data/data/io.example, where the link /data/user/0 leads (mount follows it).
    CHECK(apkrt::vfs::mount_point("/data/data/io.example/files/a") == "/data/data/io.example");
    CHECK(apkrt::vfs::mount_point("/data/data/io.example") == "/data/data/io.example");
    CHECK(apkrt::vfs::mount_point("/data/user/0") == "/data");
    // A mounted file lies in the mount of its folder, as a file in a mounted folder does on Linux.
    CHECK(apkrt::vfs::mount_point("/data/app/~~a/io.example-b/base.apk") == "/data");
    CHECK(!apkrt::vfs::mount_point("/system/lib64"));
    CHECK(!apkrt::vfs::mount_point("/dev/null"));  // only the device itself is mounted there
}

// ADR-0052: a host call starts at its mounted folder, which the table holds open; a mounted file is its host path from
// the table, with nothing below it, as iOS's sandbox lets an app open /dev/urandom but not read /dev; a mount whose
// host folder is not there locates nothing.
TEST(a_path_is_located_below_its_mounts_host_folder) {
    apkrt::vfs::reset();
    char pattern[] = "/tmp/apkrt-vfs-XXXXXX";
    const std::string host = ::mkdtemp(pattern);
    CHECK(::mkdir((host + "/data").c_str(), 0755) == 0);
    CHECK(::close(::open((host + "/base.apk").c_str(), O_CREAT | O_WRONLY, 0644)) == 0);
    apkrt::vfs::mount("/data", host + "/data");
    apkrt::vfs::mount("/data/app/~~a/io.example-b/base.apk", host + "/base.apk");
    apkrt::vfs::mount("/missing", host + "/missing");
    const auto inside = apkrt::vfs::locate("/data/user/0/io.example/files");
    CHECK(inside && inside->path == "user/0/io.example/files");
    char folder[PATH_MAX];
    CHECK(inside && ::fcntl(inside->folder, F_GETPATH, folder) == 0 && std::string(folder).ends_with("/data"));
    const auto root = apkrt::vfs::locate("/data");
    CHECK(root && root->path == "." && root->folder == inside->folder);
    const auto file = apkrt::vfs::locate("/data/app/~~a/io.example-b/base.apk");
    CHECK(file && file->folder == AT_FDCWD && file->path == host + "/base.apk" && !file->below_a_file);
    const auto below = apkrt::vfs::locate("/data/app/~~a/io.example-b/base.apk/classes.dex");
    CHECK(below && below->below_a_file);
    CHECK(!apkrt::vfs::locate("/missing/x"));
    CHECK(!apkrt::vfs::locate("/system"));
    apkrt::vfs::reset();
    static_cast<void>(std::system(("rm -rf '" + host + "'").c_str()));
}

TEST(androids_links_lead_where_android_s_do) {
    // An app's mount namespace on Android 15: /data/user/0 -> /data/data (app data isolation, Zygote.cpp), and
    // /sdcard -> /storage/self/primary -> /storage/emulated/0. A link is followed where the walk meets it, so ".."
    // after it goes up from its target, as Linux's.
    apkrt::vfs::reset();
    CHECK(absolute("/data/user/0/io.example/files", "/") == "/data/data/io.example/files");
    CHECK(absolute("/sdcard/Android/data/io.example", "/") == "/storage/emulated/0/Android/data/io.example");
    CHECK(absolute("/storage/self/primary", "/") == "/storage/emulated/0");
    CHECK(absolute("/data/user/0/../x", "/") == "/data/x");
    CHECK(absolute("0/io.example", "/data/user") == "/data/data/io.example");
    CHECK(absolute("/data/user/0", "/", false) == "/data/user/0");  // the link itself, for readlink and lstat
    CHECK(absolute("/sdcard/x", "/", false) == "/storage/emulated/0/x");
    CHECK(absolute("/data/user", "/") == "/data/user");
    // A trailing "/" or "." makes the link a folder on the way, which the walk follows (LOOKUP_FOLLOW).
    CHECK(absolute("/sdcard/", "/", false) == "/storage/emulated/0");
    CHECK(absolute("/sdcard/.", "/", false) == "/storage/emulated/0");
    CHECK(absolute("/data/user/0//", "/", false) == "/data/data");
    CHECK(absolute("/data/user/0/..", "/", false) == "/data");
    CHECK(apkrt::vfs::link_target("/data/user/0") == "/data/data");
    CHECK(apkrt::vfs::link_target("/sdcard") == "/storage/self/primary");
    CHECK(apkrt::vfs::link_target("/storage/self/primary") == "/storage/emulated/0");
    // Conscrypt's CA certificates (ADR-0046 decision 4): on Android 15 a folder of the same files, here a link.
    CHECK(apkrt::vfs::link_target("/system/etc/security/cacerts") == "/apex/com.android.conscrypt/cacerts");
    CHECK(absolute("/system/etc/security/cacerts/6187b673.0", "/") == "/apex/com.android.conscrypt/cacerts/6187b673.0");
    CHECK(!apkrt::vfs::link_target("/system/etc/security"));
    CHECK(!apkrt::vfs::link_target("/data/data"));
    CHECK(!apkrt::vfs::link_target("/data/user/0/io.example"));
}

TEST(a_long_path_takes_one_walk) {
    // A path of 200,000 components resolves in one pass over its text, not a pass per component.
    std::string path;
    for (int index = 0; index < 100'000; ++index) path += "a/../b/";
    const auto started = std::chrono::steady_clock::now();
    const auto result = absolute(path + "sdcard", "/");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(result == "/" + [] {
        std::string folders;
        for (int index = 0; index < 100'000; ++index) folders += "b/";
        return folders;
    }() + "sdcard");
}

TEST(a_mount_below_a_link_lies_at_its_target) {
    // As Linux's mount follows the links of its target: a mount of /data/user/0/<package> is one of
    // /data/data/<package>, which both names reach, and which guest_path gives back.
    apkrt::vfs::reset();
    apkrt::vfs::mount("/data/user/0/io.example", "/host/app-data");
    apkrt::vfs::mount("/sdcard/Android/data/io.example", "/host/external");
    CHECK(host_path("/data/data/io.example/files/a") == "/host/app-data/files/a");
    CHECK(guest_path("/host/app-data/files/a") == "/data/data/io.example/files/a");
    CHECK(host_path("/storage/emulated/0/Android/data/io.example") == "/host/external");
    CHECK(guest_path("/host/external/f") == "/storage/emulated/0/Android/data/io.example/f");
    apkrt::vfs::reset();
}

TEST(the_working_directory_starts_at_the_root) {
    apkrt::vfs::reset();
    CHECK(apkrt::vfs::working_directory() == "/");
    apkrt::vfs::set_working_directory("/data/local/tmp");
    CHECK(apkrt::vfs::working_directory() == "/data/local/tmp");
    apkrt::vfs::set_working_directory("/sdcard/Download");  // getcwd names the folder after the links, as d_path does
    CHECK(apkrt::vfs::working_directory() == "/storage/emulated/0/Download");
    apkrt::vfs::reset();
    CHECK(apkrt::vfs::working_directory() == "/");
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
