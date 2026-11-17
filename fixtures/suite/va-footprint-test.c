// va-footprint-test: the address space a process takes for what an engine does at its start: dlopen of a 100 MiB
// library (libva-big.so) and 64 threads with Android's default attributes (bionic's 1 MiB stack and its guard), as
// /proc/self/stat's vsize counts it before and after each step. A benchmark (tests.txt "measures"): the numbers differ
// between systems and runs; what must hold everywhere is that every thread starts and the steps return. Minato's vsize
// is the host task's virtual size, which counts the runtime, the converted libraries and each guest thread's host side.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define THREADS 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int started, released;

// Field 23 of /proc/self/stat, in bytes; 0 when it cannot be read.
static unsigned long long vsize(void) {
    FILE *file = fopen("/proc/self/stat", "r");
    if (file == NULL) return 0;
    char line[1024];
    const size_t got = fread(line, 1, sizeof line - 1, file);
    fclose(file);
    line[got] = '\0';
    const char *at = strrchr(line, ')');  // the name may hold spaces; the fields after it are plain
    if (at == NULL) return 0;
    unsigned long long value = 0;
    int field = 2;
    for (const char *p = at + 1; *p != '\0' && field < 23; ++p) {
        if (*p == ' ') ++field;
        if (field == 23) sscanf(p + 1, "%llu", &value);
    }
    return value;
}

static void *parked(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    ++started;
    pthread_cond_broadcast(&changed);
    while (!released) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static long long mib(unsigned long long after, unsigned long long before) {
    return ((long long)after - (long long)before) / (1024 * 1024);
}

int main(void) {
    const unsigned long long start = vsize();
    printf("vsize at start: %llu MiB\n", start / (1024 * 1024));
    void *big = dlopen("libva-big.so", RTLD_NOW);
    const unsigned long long loaded = vsize();
    printf("dlopen of the 100 MiB library: %s, %+lld MiB\n", big != NULL ? "loaded" : dlerror(), mib(loaded, start));

    pthread_t threads[THREADS];
    int failures = 0, made = 0;
    for (int index = 0; index < THREADS; ++index) {
        if (pthread_create(&threads[index], NULL, parked, NULL) == 0) {
            ++made;
        } else {
            ++failures;
        }
    }
    pthread_mutex_lock(&lock);
    while (started < made) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    const unsigned long long parked_all = vsize();
    printf("%d threads parked, %d failed: %+lld MiB, %lld KiB a thread\n", made, failures, mib(parked_all, loaded),
           made > 0 ? ((long long)parked_all - (long long)loaded) / 1024 / made : 0);

    pthread_mutex_lock(&lock);
    released = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (int index = 0; index < made; ++index) pthread_join(threads[index], NULL);
    printf("after they ended: %+lld MiB against the start\n", mib(vsize(), start));
    return failures == 0 && big != NULL ? 0 : 1;
}
