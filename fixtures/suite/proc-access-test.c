// access() and stat()/lstat() on the /proc names the runtime serves (proc.cpp) and the directories /proc,
// /proc/self and /proc/self/task, printed the same way on Android and on Minato (ADR-0033 decision 8). Mono's
// mini_init does access("/proc/self/maps", F_OK), and root checks probe /proc, so the answers must be Android's.
// What this does not cover, and so does not probe: following a /proc symlink (stat and access follow exe, cwd,
// root and fd/<n> to their targets, which differ per host); /proc's own link count (the process count, per system).
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { WANT_ACCESS = 1, WANT_LSTAT = 2, WANT_STAT = 4, NO_NLINK = 8 };

static const char *type_of(mode_t m) {
    if (S_ISREG(m)) return "reg";
    if (S_ISDIR(m)) return "dir";
    if (S_ISLNK(m)) return "lnk";
    if (S_ISCHR(m)) return "chr";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISSOCK(m)) return "sock";
    return "?";
}

static const char *acc(int r) {
    if (r == 0) return "ok";
    if (errno == EACCES) return "EACCES";
    if (errno == ENOENT) return "ENOENT";
    return "err";
}

static void field(const char *name, int ok, const struct stat *s, unsigned flags) {
    if (!ok) {
        printf(" %s=ENOENT", name);
        return;
    }
    if (flags & NO_NLINK) printf(" %s=%s/%04o", name, type_of(s->st_mode), s->st_mode & 07777);
    else printf(" %s=%s/%04o/%lu", name, type_of(s->st_mode), s->st_mode & 07777, (unsigned long)s->st_nlink);
}

static void probe(const char *path, unsigned flags) {
    printf("%s:", path);
    if (flags & WANT_ACCESS) printf(" F_OK=%s R_OK=%s", acc(access(path, F_OK)), acc(access(path, R_OK)));
    struct stat ls;
    if (flags & WANT_LSTAT) field("lstat", lstat(path, &ls) == 0, &ls, flags);
    struct stat ss;
    if (flags & WANT_STAT) field("stat", stat(path, &ss) == 0, &ss, flags);
    printf("\n");
}

int main(void) {
    // The regular files: they exist, are readable, and are reg/0444 with one link.
    const char *regs[] = {"/proc/cpuinfo",      "/proc/meminfo",     "/proc/self/stat",
                          "/proc/self/maps",    "/proc/self/smaps",  "/proc/self/cmdline"};
    for (unsigned i = 0; i < sizeof regs / sizeof *regs; i++) probe(regs[i], WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    // The directories. /proc's link count is the process count, so it is not shown; /proc/self is a symlink by
    // lstat and the process directory by stat; /proc/self/task counts two plus this one thread.
    probe("/proc", WANT_ACCESS | WANT_LSTAT | WANT_STAT | NO_NLINK);
    probe("/proc/self", WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    probe("/proc/self/task", WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    // The symlinks the runtime serves, by lstat alone (following them is out of scope). A file descriptor link
    // sits at a fixed number so the output does not depend on which descriptor the open returns.
    probe("/proc/self/exe", WANT_LSTAT);
    probe("/proc/self/cwd", WANT_LSTAT);
    probe("/proc/self/root", WANT_LSTAT);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0 && fd != 7) {
        dup2(fd, 7);
        close(fd);
    }
    probe("/proc/self/fd/7", WANT_LSTAT);
    // A name under /proc the runtime does not serve: absent here, as it is on Android for a name that is not there.
    probe("/proc/self/not-a-real-entry", WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    return 0;
}
