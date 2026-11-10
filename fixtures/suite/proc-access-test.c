// access() and stat()/lstat() on the /proc names the runtime serves (proc.cpp) and the directories /proc,
// /proc/self and /proc/self/task, printed the same way on Android and on Minato (ADR-0033 decision 8). Mono's
// mini_init does access("/proc/self/maps", F_OK), and root checks probe /proc, so the answers must be Android's.
// The symlinks exe, cwd, root and fd/<n> are shown by lstat, and -- since stat and access follow them -- also
// (cwd and fd) stat()ed and compared with the target reached directly, so the line is the same on both.
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

// Whether stat(path), which follows it, names the same file (device and inode) as `direct`, with the type; so a
// followed /proc link answers the target's stat, as it does on Android.
static void follows(const char *path, const char *direct) {
    struct stat a, b;
    if (stat(path, &a) != 0) {
        printf("%s follows: %s\n", path, acc(-1));
        return;
    }
    if (stat(direct, &b) != 0) {
        printf("%s follows: the target is %s\n", path, acc(-1));
        return;
    }
    const int same = a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    printf("%s follows: %s (%s)\n", path, same ? "same file" : "different file", type_of(a.st_mode));
}

int main(void) {
    // A regular file at a fixed descriptor number for the fd/<n> probes; unlinked, so it leaves nothing behind.
    int fd = open("proc-access-fd", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0 && fd != 7) {
        dup2(fd, 7);
        close(fd);
    }
    unlink("proc-access-fd");
    // The regular files: they exist, are readable, and are reg/0444 with one link.
    const char *regs[] = {"/proc/cpuinfo",      "/proc/meminfo",     "/proc/self/stat",
                          "/proc/self/maps",    "/proc/self/smaps",  "/proc/self/cmdline"};
    for (unsigned i = 0; i < sizeof regs / sizeof *regs; i++) probe(regs[i], WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    // The directories. /proc's link count is the process count, so it is not shown; /proc/self is a symlink by
    // lstat and the process directory by stat; /proc/self/task counts two plus this one thread.
    probe("/proc", WANT_ACCESS | WANT_LSTAT | WANT_STAT | NO_NLINK);
    probe("/proc/self", WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    probe("/proc/self/task", WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    // The symlinks the runtime serves, by lstat.
    probe("/proc/self/exe", WANT_LSTAT);
    probe("/proc/self/cwd", WANT_LSTAT);
    probe("/proc/self/root", WANT_LSTAT);
    probe("/proc/self/fd/7", WANT_LSTAT);
    // A name under /proc the runtime does not serve: absent here, as it is on Android for a name that is not there.
    probe("/proc/self/not-a-real-entry", WANT_ACCESS | WANT_LSTAT | WANT_STAT);
    // Followed symlinks: stat answers the target, compared with the target reached directly. Only cwd and the
    // descriptor are checked here: this test harness mounts no "/" and the executable is not a plain file in it, so
    // their targets are unreachable on the Mac though they are real on the device; cwd exercises the same code path.
    follows("/proc/self/cwd", ".");
    // fd/7 follows to the descriptor: compare with fstat of it.
    struct stat a, b;
    if (stat("/proc/self/fd/7", &a) == 0 && fstat(7, &b) == 0) {
        const int same = a.st_dev == b.st_dev && a.st_ino == b.st_ino;
        printf("/proc/self/fd/7 follows: %s (%s)\n", same ? "same file" : "different file", type_of(a.st_mode));
    } else {
        printf("/proc/self/fd/7 follows: %s\n", acc(-1));
    }
    return 0;
}
