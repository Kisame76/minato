// proc-links-test: the links exe, cwd and root of /proc/self and /proc/<pid>, as Android 15's kernel shows them, which
// Unity and FMOD read (the Unity test APK probe): the program's file, the working directory before and after chdir, the root,
// a target cut to readlink's size, and an entry that does not exist. The working directory moves into a folder of
// its own and back, as the fixture's runs mount no root to move to.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void show(const char *label, const char *path, size_t size) {
    char target[256];
    const ssize_t length = readlink(path, target, size);
    if (length < 0) {
        printf("%s: %s\n", label, errno == ENOENT ? "ENOENT" : strerror(errno));
        return;
    }
    printf("%s: %.*s\n", label, (int)length, target);
}

int main(void) {
    char path[64];
    show("exe", "/proc/self/exe", 256);
    snprintf(path, sizeof path, "/proc/%d/exe", getpid());
    show("exe by pid", path, 256);
    show("exe cut to 7", "/proc/self/exe", 7);
    show("cwd at the start", "/proc/self/cwd", 256);
    rmdir("proc-links-folder");
    if (mkdir("proc-links-folder", 0700) != 0) perror("mkdir proc-links-folder");
    if (chdir("proc-links-folder") != 0) perror("chdir proc-links-folder");
    show("cwd after chdir to a folder", "/proc/self/cwd", 256);
    if (chdir("..") != 0) perror("chdir ..");
    show("cwd after chdir ..", "/proc/self/cwd", 256);
    rmdir("proc-links-folder");
    show("root", "/proc/self/root", 256);
    show("nothing", "/proc/self/nothing", 256);
    return 0;
}
