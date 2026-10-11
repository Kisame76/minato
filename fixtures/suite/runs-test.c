// A library whose code, read-only data and data follow its data (ADR-0008 amendment, the Unity test APK v2's
// libil2cpp.so): what libruns-lib.so computes on both sides, where dl_iterate_phdr says its segments lie, and what
// /proc/self/maps says about it. Every line is the same on Android.
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int runs_counter;
extern int runs_late_counter;
extern const char runs_late_text[];
int runs_constructed(void);
int runs_late_next(void);
int runs_apply(int index, int value);
const char *runs_text(int index);
int *runs_thread_address(void);

static const char library[] = "libruns-lib.so";

static int is_library(const char *path) {
    const size_t length = strlen(path), name = sizeof library - 1;
    return length >= name && strcmp(path + length - name, library) == 0;
}

static int inside(uintptr_t address, uintptr_t start, uintptr_t end) {
    return address >= start && address < end;
}

struct Segments {
    uintptr_t bias;
    int loads;
    char flags[12][4];
    int late_code_executable, late_counter_writable, late_text_read_only;
};

static int find_segments(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (info->dlpi_name == NULL || !is_library(info->dlpi_name)) return 0;
    struct Segments *segments = data;
    segments->bias = info->dlpi_addr;
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[index];
        if (header->p_type != PT_LOAD) continue;
        if (segments->loads < 12) {
            char *flags = segments->flags[segments->loads];
            flags[0] = (header->p_flags & PF_R) ? 'r' : '-';
            flags[1] = (header->p_flags & PF_W) ? 'w' : '-';
            flags[2] = (header->p_flags & PF_X) ? 'x' : '-';
            flags[3] = '\0';
        }
        ++segments->loads;
        const uintptr_t start = info->dlpi_addr + header->p_vaddr, end = start + header->p_memsz;
        if ((header->p_flags & PF_X) && inside((uintptr_t)&runs_late_next, start, end)) {
            segments->late_code_executable = 1;
        }
        if ((header->p_flags & PF_W) && inside((uintptr_t)&runs_late_counter, start, end)) {
            segments->late_counter_writable = 1;
        }
        if (!(header->p_flags & PF_W) && inside((uintptr_t)runs_late_text, start, end)) {
            segments->late_text_read_only = 1;
        }
    }
    return 1;
}

static void *other_thread(void *unused) {
    (void)unused;
    int *value = runs_thread_address();
    const int before = *value;
    *value = 9;
    return (void *)(intptr_t)before;
}

// The library's lines of /proc/self/maps: in order and apart, the first at the load address with file offset 0, the
// code after the data in an executable line and its counter in a writable one.
static void check_maps(uintptr_t bias) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL) {
        printf("maps: cannot open\n");
        return;
    }
    char line[512];
    int lines = 0, ordered = 1, first_at_bias = 0, code_executable = 0, counter_writable = 0;
    uintptr_t previous_end = 0;
    while (fgets(line, sizeof line, maps) != NULL) {
        unsigned long begin, end, offset;
        char permissions[5], path[256] = "";
        if (sscanf(line, "%lx-%lx %4s %lx %*s %*s %255s", &begin, &end, permissions, &offset, path) < 4) continue;
        if (!is_library(path)) continue;
        if (lines++ == 0) first_at_bias = begin == bias && offset == 0;
        if (begin < previous_end || end <= begin) ordered = 0;
        previous_end = end;
        if (inside((uintptr_t)&runs_late_next, begin, end) && permissions[2] == 'x') code_executable = 1;
        if (inside((uintptr_t)&runs_late_counter, begin, end) && permissions[1] == 'w') counter_writable = 1;
    }
    fclose(maps);
    printf("maps: lines in order and apart %s, the first at the load address with offset 0 %s, the code after the "
           "data in an executable line %s, its counter in a writable line %s\n",
           lines > 0 && ordered ? "yes" : "no", first_at_bias ? "yes" : "no", code_executable ? "yes" : "no",
           counter_writable ? "yes" : "no");
}

int main(void) {
    printf("constructor in the code after the data: %d\n", runs_constructed());
    const int late_first = runs_late_counter, late_next = runs_late_next(), late_again = runs_late_next();
    printf("counters: after the data %d, then %d and %d; before it %d\n", late_first, late_next, late_again,
           runs_counter);
    void *handle = dlopen(library, RTLD_NOW);
    const void *next = handle != NULL ? dlsym(handle, "runs_late_next") : NULL;
    const void *counter = handle != NULL ? dlsym(handle, "runs_late_counter") : NULL;
    printf("dlsym finds the same: code %s, counter %s\n", next == (const void *)&runs_late_next ? "yes" : "no",
           counter == (const void *)&runs_late_counter ? "yes" : "no");
    printf("operations: %d %d\n", runs_apply(0, 3), runs_apply(1, 3));
    printf("texts: %s, %s; directly: %s\n", runs_text(0), runs_text(1), runs_late_text);

    pthread_t thread;
    void *other_before = NULL;
    pthread_create(&thread, NULL, other_thread, NULL);
    pthread_join(thread, &other_before);
    printf("thread values: main %d, the other thread's before its write %d, main after it %d\n",
           *runs_thread_address(), (int)(intptr_t)other_before, *runs_thread_address());

    struct Segments segments = {0};
    dl_iterate_phdr(find_segments, &segments);
    printf("program headers: %d LOAD segments:", segments.loads);
    for (int index = 0; index < segments.loads && index < 12; ++index) printf(" %s", segments.flags[index]);
    printf("\nafter the data: the code in an executable LOAD %s, its counter in a writable one %s, its text in a "
           "read-only one %s\n",
           segments.late_code_executable ? "yes" : "no", segments.late_counter_writable ? "yes" : "no",
           segments.late_text_read_only ? "yes" : "no");
    check_maps(segments.bias);
    if (handle != NULL) dlclose(handle);
    return 0;
}
