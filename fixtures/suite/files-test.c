// files-test: files and directories (M2 step 5c) through bionic's stdio, the POSIX calls and
// dirent, in the working directory, which Android's shell and the runtime both make /data/local/tmp.
// Errors are checked by their Linux errno values. Expected output: fixtures/expected/files-test.out
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void report(const char *what, int result) {
    if (result == -1) {
        printf("%s: -1 errno %d (%s)\n", what, errno, strerror(errno));
    } else {
        printf("%s: %d\n", what, result);
    }
}

static int compare_names(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static void standard_io(void) {
    FILE *file = fopen("notes.txt", "w");
    fprintf(file, "line %d\n", 1);
    fputs("line 2\n", file);
    fclose(file);
    file = fopen("notes.txt", "a");
    fputs("line 3\n", file);
    fclose(file);

    file = fopen("notes.txt", "r");
    char line[64];
    while (fgets(line, sizeof line, file) != NULL) printf("fgets: %s", line);
    printf("feof: %d\n", feof(file) != 0);
    fclose(file);

    file = fopen("notes.txt", "r+");
    fseek(file, 0, SEEK_END);
    printf("ftell at the end: %ld\n", ftell(file));
    fseek(file, 5, SEEK_SET);
    fputc('X', file);
    rewind(file);
    fgets(line, sizeof line, file);
    printf("after fputc: %s", line);
    fclose(file);

    FILE *missing = fopen("missing.txt", "r");
    printf("fopen of a missing file: %s, errno %d\n", missing == NULL ? "NULL" : "a FILE", errno);

    FILE *temporary = tmpfile();
    fputs("temporary", temporary);
    rewind(temporary);
    printf("tmpfile: %s\n", fgets(line, sizeof line, temporary));
    fclose(temporary);
}

static void descriptors(void) {
    umask(022);
    const int fd = open("data.bin", O_CREAT | O_RDWR | O_TRUNC, 0666);
    printf("write: %zd\n", write(fd, "0123456789", 10));
    char buffer[16] = "";
    lseek(fd, 2, SEEK_SET);
    printf("read after lseek: %.*s\n", (int)read(fd, buffer, 3), buffer);
    printf("pread: %.*s\n", (int)pread(fd, buffer, 2, 8), buffer);
    pwrite(fd, "AB", 2, 0);
    lseek(fd, 0, SEEK_SET);
    printf("after pwrite: %.*s\n", (int)read(fd, buffer, 10), buffer);
    report("ftruncate", ftruncate(fd, 4));
    struct stat status;
    fstat(fd, &status);
    printf("fstat: size %lld, regular %d, mode %o\n", (long long)status.st_size, S_ISREG(status.st_mode),
           status.st_mode & 0777);
    report("fsync", fsync(fd));
    close(fd);
    report("close again", close(fd));
    report("stat of a missing file", stat("missing", &status));
    report("open of a missing file", open("missing", O_RDONLY));
    report("open below a file", open("data.bin/below", O_RDONLY));
    report("open O_EXCL of an existing file", open("data.bin", O_CREAT | O_EXCL | O_WRONLY, 0600));
}

static void directories(void) {
    report("mkdir", mkdir("dir", 0755));
    report("mkdir again", mkdir("dir", 0755));
    report("open a directory for writing", open("dir", O_WRONLY));
    const char *files[] = {"dir/b", "dir/a", "dir/c"};
    for (int i = 0; i < 3; ++i) close(open(files[i], O_CREAT | O_WRONLY, 0644));
    mkdir("dir/sub", 0700);

    DIR *directory = opendir("dir");
    char *names[16];
    int count = 0, all = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        ++all;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char *name = malloc(strlen(entry->d_name) + 8);
        sprintf(name, "%s:%s", entry->d_name, entry->d_type == DT_DIR ? "dir" : entry->d_type == DT_REG ? "file" : "?");
        names[count++] = name;
    }
    qsort(names, (size_t)count, sizeof names[0], compare_names);
    printf("readdir (%d entries with . and ..):", all);
    for (int i = 0; i < count; ++i) {
        printf(" %s", names[i]);
        free(names[i]);
    }
    printf("\n");
    rewinddir(directory);
    int again = 0;
    while (readdir(directory) != NULL) ++again;
    printf("after rewinddir: %d entries\n", again);
    closedir(directory);

    report("rename", rename("dir/a", "dir/renamed"));
    report("access of the old name", access("dir/a", F_OK));
    report("access of the new name", access("dir/renamed", F_OK));
    report("rmdir of a full directory", rmdir("dir"));
    report("unlink of a directory", unlink("dir/sub"));
    report("rmdir of a file", rmdir("dir/b"));
    report("remove of a directory", remove("dir/sub"));
    const char *left[] = {"dir/b", "dir/c", "dir/renamed"};
    for (int i = 0; i < 3; ++i) unlink(left[i]);
    report("rmdir of the empty directory", rmdir("dir"));
    struct stat status;
    report("stat after rmdir", stat("dir", &status));
}

static void working_directory(void) {
    char path[256];
    printf("getcwd: %s\n", getcwd(path, sizeof path));
    mkdir("nested", 0755);
    report("chdir", chdir("nested"));
    printf("getcwd: %s\n", getcwd(path, sizeof path));
    close(open("inner", O_CREAT | O_WRONLY, 0644));
    report("chdir ..", chdir(".."));
    printf("getcwd: %s\n", getcwd(path, sizeof path));
    report("access by the longer path", access("nested/inner", F_OK));
    report("access by the absolute path", access("/data/local/tmp/nested/inner", R_OK));
    report("chdir into a file", chdir("notes.txt"));
    report("access of a missing path", access("/data/local/nonexistent", F_OK));
}

static void pipes_and_devices(void) {
    int ends[2];
    report("pipe", pipe(ends));
    char buffer[16] = "";
    write(ends[1], "pipe", 4);
    printf("read from the pipe: %.*s\n", (int)read(ends[0], buffer, sizeof buffer), buffer);
    fcntl(ends[0], F_SETFL, fcntl(ends[0], F_GETFL) | O_NONBLOCK);
    report("read from an empty non-blocking pipe", (int)read(ends[0], buffer, sizeof buffer));
    report("dup2", dup2(ends[1], 20));
    write(20, "x", 1);
    printf("read through dup2: %.*s\n", (int)read(ends[0], buffer, sizeof buffer), buffer);
    close(20);
    close(ends[0]);
    close(ends[1]);

    const int null = open("/dev/null", O_RDWR);
    printf("/dev/null: write %zd, read %zd\n", write(null, "gone", 4), read(null, buffer, sizeof buffer));
    close(null);
    const int zero = open("/dev/zero", O_RDONLY);
    memset(buffer, 0xff, sizeof buffer);
    const ssize_t zeros = read(zero, buffer, 8);
    int all_zero = 1;
    for (int i = 0; i < 8; ++i) all_zero &= buffer[i] == 0;
    printf("/dev/zero: read %zd, all zero %d\n", zeros, all_zero);
    close(zero);
    const int random = open("/dev/urandom", O_RDONLY);
    printf("/dev/urandom: read %zd\n", read(random, buffer, 16));
    close(random);
}

static void links(void) {
    report("symlink", symlink("notes.txt", "link"));
    char target[64] = "";
    const ssize_t length = readlink("link", target, sizeof target);
    printf("readlink: %zd %.*s\n", length, (int)length, target);
    struct stat status;
    lstat("link", &status);
    printf("lstat: link %d\n", S_ISLNK(status.st_mode));
    stat("link", &status);
    printf("stat through the link: regular %d, size %lld\n", S_ISREG(status.st_mode), (long long)status.st_size);
    report("readlink of a file", (int)readlink("notes.txt", target, sizeof target));
    report("unlink the link", unlink("link"));
}

int main(void) {
    standard_io();
    descriptors();
    directories();
    working_directory();
    pipes_and_devices();
    links();
    return 0;
}
