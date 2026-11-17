// jit-fallback-test: a library's own JIT, as LuaJIT's and PCRE2's try it, in the three shapes they use for executable
// memory: an anonymous mapping that is readable, writable and executable at once; an anonymous one written, then
// turned readable and executable with mprotect; and one memfd mapped twice, writable and executable. Where a shape is
// given, the program writes `mov w0, #42; ret` into it and calls it; where it is refused, it computes the same 42 with
// its interpreter, as such a library falls back. Android gives every shape; Minato gives none (only signed code runs,
// ADR-0002) and answers EACCES, as Android's policy does when it refuses executable memory. Either way the program
// prints the same lines and never crashes.
// Expected output: fixtures/expected/jit-fallback-test.out (Android 15's).
#define _GNU_SOURCE
#include <errno.h>
#include <linux/memfd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static const uint32_t code[] = {0x52800540, 0xd65f03c0};  // mov w0, #42; ret
static int refusals, other_errors;

// The fallback: the same 6 * 7 without generated code, as an interpreter computes it.
static int interpret(void) {
    static const int program[] = {6, 7};
    return program[0] * program[1];
}

static int run(void *executable) {
    __builtin___clear_cache((char *)executable, (char *)executable + sizeof code);
    return ((int (*)(void))executable)();
}

static void refused(void) {
    if (errno == EACCES) ++refusals;
    else ++other_errors;
}

static int rwx_anonymous(size_t page) {
    void *memory = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        refused();
        return interpret();
    }
    memcpy(memory, code, sizeof code);
    const int result = run(memory);
    munmap(memory, page);
    return result;
}

static int rw_then_rx(size_t page) {
    void *memory = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) return -1;
    memcpy(memory, code, sizeof code);
    int result;
    if (mprotect(memory, page, PROT_READ | PROT_EXEC) != 0) {
        refused();
        result = interpret();
    } else {
        result = run(memory);
    }
    munmap(memory, page);
    return result;
}

static int memfd_dual(size_t page) {
    const int fd = (int)syscall(__NR_memfd_create, "jit", MFD_CLOEXEC);  // libc's wrapper is API 30
    if (fd < 0 || ftruncate(fd, (off_t)page) != 0) return -1;
    void *writable = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (writable == MAP_FAILED) {
        close(fd);
        return -1;
    }
    memcpy(writable, code, sizeof code);
    void *executable = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    int result;
    if (executable == MAP_FAILED) {
        refused();
        result = interpret();
    } else {
        result = run(executable);
        munmap(executable, page);
    }
    munmap(writable, page);
    close(fd);
    return result;
}

int main(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    printf("rwx anonymous: %d\n", rwx_anonymous(page));
    printf("rw, then rx: %d\n", rw_then_rx(page));
    printf("memfd mapped twice: %d\n", memfd_dual(page));
    printf("every refusal EACCES: %s\n", other_errors == 0 ? "yes" : "no");
    // What is left of the address space works as before: a plain mapping still reads and writes.
    char *data = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data != MAP_FAILED) data[0] = 'x';
    printf("plain mapping after: %s\n", data != MAP_FAILED && data[0] == 'x' ? "writable" : "broken");
    (void)refusals;
    return 0;
}
