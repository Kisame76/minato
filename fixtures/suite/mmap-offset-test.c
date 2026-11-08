// mmap-offset-test: a file mapping whose offset is 4 KiB-aligned but not 16 KiB-aligned, as an engine makes when it
// mmaps an uncompressed APK/OBB entry at the file offset AAsset_openFileDescriptor64 returns (ADR-0007 change,
// 2026-11-07). Android zipaligns stored entries to 4 KiB, so such an offset is common; a 4 KiB kernel accepts the
// mapping, a 16 KiB kernel (and iOS) answers EINVAL, and engines then fall back to reading the whole file into
// anonymous memory -- a multi-GB memory blow-up. Minato emulates it (maps the 16 KiB-aligned region and returns
// the address plus the delta), so the mapping works and stays file-backed. Every line is fixed by the bytes.
// Expected output: fixtures/expected/mmap-offset-test.out
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { k4k = 4096, k16k = 16384 };

// A byte that depends on the file position, so a mapping at the wrong offset shows at once.
static unsigned char at(size_t position) {
    return (unsigned char)(position * 131 + (position >> 8) * 17 + 5);
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

// Maps `length` bytes of `fd` at `offset`, checks the bytes are the file's at that offset, unmaps, and prints one line.
static void map_and_check(const char *label, int fd, off_t offset, size_t length) {
    errno = 0;
    unsigned char *map = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, offset);
    if (map == MAP_FAILED) {
        printf("%s: mmap failed, errno %d\n", label, errno);
        return;
    }
    int same = 1;
    for (size_t index = 0; index < length; ++index)
        if (map[index] != at((size_t)offset + index)) { same = 0; break; }
    // Exercise the accounting the emulation must keep right: read-only mprotect of the mapping, then unmap it.
    const int protect = mprotect(map, length, PROT_READ);
    const int unmap = munmap(map, length);
    printf("%s: mapped, bytes %s, mprotect %d, munmap %d\n", label, same ? "match" : "DIFFER", protect, unmap);
}

// Writes a file of `size` bytes with the position pattern; returns an open read-only fd, or -1.
static int write_pattern_file(const char *path, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    unsigned char *bytes = malloc(size);
    for (size_t index = 0; index < size; ++index) bytes[index] = at(index);
    const ssize_t written = write(fd, bytes, size);
    free(bytes);
    close(fd);
    return written == (ssize_t)size ? open(path, O_RDONLY) : -1;
}

// Writes a minimal APK (ZIP) with one stored entry whose data begins at `data_offset` (4 KiB-aligned, via the local
// header's extra field, as zipalign pads), filled with the position pattern; returns an open read-only fd.
static int write_apk_with_stored_entry(const char *path, off_t data_offset, size_t data_size) {
    const char name[] = "assets/s.bin";
    const size_t name_size = sizeof name - 1;
    const size_t header = 30 + name_size;           // local file header + name
    const size_t extra = (size_t)data_offset - header;  // pad with an extra field so data lands at data_offset
    const size_t total = (size_t)data_offset + data_size;
    unsigned char *bytes = calloc(1, total);
    put32(bytes, 0x04034b50);           // local file header signature
    put16(bytes + 8, 0);                // method 0: stored
    put32(bytes + 18, (uint32_t)data_size);  // compressed size
    put32(bytes + 22, (uint32_t)data_size);  // uncompressed size
    put16(bytes + 26, (unsigned)name_size);
    put16(bytes + 28, (unsigned)extra);
    memcpy(bytes + 30, name, name_size);
    // The extra field: a padding record (id 0xCAFE), whose contents do not matter to a mmap of the data.
    put16(bytes + header, 0xcafe);
    put16(bytes + header + 2, (unsigned)(extra >= 4 ? extra - 4 : 0));
    for (size_t index = 0; index < data_size; ++index)
        bytes[(size_t)data_offset + index] = at((size_t)data_offset + index);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const ssize_t written = fd >= 0 ? write(fd, bytes, total) : -1;
    if (fd >= 0) close(fd);
    free(bytes);
    return written == (ssize_t)total ? open(path, O_RDONLY) : -1;
}

int main(void) {
    const int fd = write_pattern_file("/data/local/tmp/mmap-offset.dat", 64 * 1024);
    if (fd < 0) { printf("cannot write the data file\n"); return 1; }
    map_and_check("file at a 16K offset", fd, k16k, k4k);       // control: always works
    map_and_check("file at a 4K offset", fd, k4k, k4k);         // the emulated case
    map_and_check("file at an 8K offset", fd, 2 * k4k, k4k);    // another non-16K offset
    close(fd);

    const int apk = write_apk_with_stored_entry("/data/local/tmp/mmap-offset.apk", k4k, 100);
    if (apk < 0) { printf("cannot write the APK\n"); return 1; }
    map_and_check("APK stored asset at a 4K offset", apk, k4k, 100);
    close(apk);

    printf("done\n");
    return 0;
}
