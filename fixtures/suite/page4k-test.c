// A library laid out for 4 KiB pages whose code and writable data meet in one 16 KiB page (ADR-0007
// decision 2, as amended): what libpage4k-lib.so computes, where dl_iterate_phdr says its segments lie,
// and what /proc/self/maps says about it. Every line is the same on a 4 KiB Android device.
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int page4k_counter;
int page4k_constructed(void);
int page4k_next(void);
int page4k_apply(int index, int value);
const char *page4k_name(int index);
int *page4k_thread_address(void);

static const char library[] = "libpage4k-lib.so";

static int is_library(const char *path) {
    const size_t length = strlen(path), name = sizeof library - 1;
    return length >= name && strcmp(path + length - name, library) == 0;
}

struct Segments {
    uintptr_t bias;
    int loads;
    char flags[8][4];
    int shared_page;                         // the last executable LOAD ends in the first writable one's 16 KiB page
    int counter_writable, code_executable;   // where the counter and page4k_next lie
};

static int find_segments(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (info->dlpi_name == NULL || !is_library(info->dlpi_name)) return 0;
    struct Segments *segments = data;
    segments->bias = info->dlpi_addr;
    uintptr_t code_end = 0, data_start = UINTPTR_MAX;
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[index];
        if (header->p_type != PT_LOAD) continue;
        if (segments->loads < 8) {
            char *flags = segments->flags[segments->loads];
            flags[0] = (header->p_flags & PF_R) ? 'r' : '-';
            flags[1] = (header->p_flags & PF_W) ? 'w' : '-';
            flags[2] = (header->p_flags & PF_X) ? 'x' : '-';
            flags[3] = '\0';
        }
        ++segments->loads;
        const uintptr_t start = info->dlpi_addr + header->p_vaddr, end = start + header->p_memsz;
        if ((header->p_flags & PF_X) && header->p_vaddr + header->p_memsz > code_end) {
            code_end = header->p_vaddr + header->p_memsz;
        }
        if ((header->p_flags & PF_W) && header->p_vaddr < data_start) data_start = header->p_vaddr;
        const uintptr_t counter = (uintptr_t)&page4k_counter, code = (uintptr_t)&page4k_next;
        if ((header->p_flags & PF_W) && counter >= start && counter < end) segments->counter_writable = 1;
        if ((header->p_flags & PF_X) && code >= start && code < end) segments->code_executable = 1;
    }
    segments->shared_page = data_start != UINTPTR_MAX && (code_end - 1) / 16384 == data_start / 16384;
    return 1;
}

static void *other_thread(void *unused) {
    (void)unused;
    int *value = page4k_thread_address();
    const int before = *value;
    *value = 9;
    return (void *)(intptr_t)before;
}

// The library's lines of /proc/self/maps: in order and apart, the first at the load address with file offset
// 0, the counter in a writable line and the code in an executable one.
static void check_maps(uintptr_t bias) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        printf("maps: cannot open\n");
        return;
    }
    char line[512];
    int lines = 0, ordered = 1, first_at_bias = 0, counter_writable = 0, code_executable = 0;
    uintptr_t previous_end = 0;
    while (fgets(line, sizeof line, maps) != NULL) {
        unsigned long begin, end, offset;
        char permissions[5], path[256] = "";
        if (sscanf(line, "%lx-%lx %4s %lx %*s %*s %255s", &begin, &end, permissions, &offset, path) < 4) continue;
        if (!is_library(path)) continue;
        if (lines++ == 0) first_at_bias = begin == bias && offset == 0;
        if (begin < previous_end || end <= begin) ordered = 0;
        previous_end = end;
        const uintptr_t counter = (uintptr_t)&page4k_counter, code = (uintptr_t)&page4k_next;
        if (counter >= begin && counter < end && permissions[1] == 'w') counter_writable = 1;
        if (code >= begin && code < end && permissions[2] == 'x') code_executable = 1;
    }
    fclose(maps);
    printf("maps: lines in order and apart %s, the first at the load address with offset 0 %s, the counter in a "
           "writable line %s, the code in an executable line %s\n",
           lines > 0 && ordered ? "yes" : "no", first_at_bias ? "yes" : "no", counter_writable ? "yes" : "no",
           code_executable ? "yes" : "no");
}

int main(void) {
    printf("constructor: %d\n", page4k_constructed());
    const int first = page4k_counter, next = page4k_next();
    void *handle = dlopen(library, RTLD_NOW);
    const int *found = handle != NULL ? dlsym(handle, "page4k_counter") : NULL;
    printf("counter: %d, then %d; dlsym finds the same: %s\n", first, next, found == &page4k_counter ? "yes" : "no");
    printf("operations: %d %d\n", page4k_apply(0, 3), page4k_apply(1, 3));
    printf("names: %s %s %s\n", page4k_name(0), page4k_name(1), page4k_name(2));

    pthread_t thread;
    void *other_before = NULL;
    pthread_create(&thread, NULL, other_thread, NULL);
    pthread_join(thread, &other_before);
    printf("thread values: main %d, the other thread's before its write %d, main after it %d\n",
           *page4k_thread_address(), (int)(intptr_t)other_before, *page4k_thread_address());

    struct Segments segments = {0};
    dl_iterate_phdr(find_segments, &segments);
    printf("program headers: %d LOAD segments:", segments.loads);
    for (int index = 0; index < segments.loads && index < 8; ++index) printf(" %s", segments.flags[index]);
    printf("; code and writable data share a 16 KiB page: %s\n", segments.shared_page ? "yes" : "no");
    printf("the counter in a writable LOAD: %s, the code in an executable LOAD: %s\n",
           segments.counter_writable ? "yes" : "no", segments.code_executable ? "yes" : "no");
    check_maps(segments.bias);
    if (handle != NULL) dlclose(handle);
    return 0;
}
