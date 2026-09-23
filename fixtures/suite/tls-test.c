// tls-test: ELF thread-local storage (M2 step 5c): the program's own __thread variables (local-exec),
// a library's, reached from the library (TLS descriptors) and from the program (initial-exec), with
// initial values, zero-initialized and over-aligned variables, an undefined weak one, and a separate
// copy for every thread. Expected output: fixtures/expected/tls-test.out
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

__thread int exe_counter = 5;
__thread long exe_zero;
extern __thread int lib_counter;

int lib_increment(int amount);
int *lib_counter_address(void);
int lib_zero_value(void);
int lib_aligned_ok(void);
int lib_weak_is_null(void);

struct Seen {
    int index;
    int exe_start, lib_start;
    int exe_end, lib_end;
};

static void *work(void *argument) {
    struct Seen *seen = argument;
    seen->exe_start = exe_counter;
    seen->lib_start = lib_counter;
    for (int i = 0; i < 1000; ++i) {
        exe_counter += seen->index;
        lib_increment(seen->index);
    }
    seen->exe_end = exe_counter;
    seen->lib_end = *lib_counter_address();
    return NULL;
}

int main(void) {
    printf("program __thread: %d, zero %ld\n", exe_counter, exe_zero);
    printf("library __thread from the library: %d, from the program: %d, same address: %s\n", lib_increment(0),
           lib_counter, lib_counter_address() == &lib_counter ? "yes" : "no");
    printf("library zero-initialized: %d, 64-byte aligned: %s, undefined weak is NULL: %s\n", lib_zero_value(),
           lib_aligned_ok() ? "yes" : "no", lib_weak_is_null() ? "yes" : "no");

    struct Seen seen[4];
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) {
        seen[i] = (struct Seen){.index = i + 1};
        pthread_create(&threads[i], NULL, work, &seen[i]);
    }
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    for (int i = 0; i < 4; ++i) {
        printf("thread %d: started with %d and %d, ended with %d and %d\n", seen[i].index, seen[i].exe_start,
               seen[i].lib_start, seen[i].exe_end, seen[i].lib_end);
    }
    printf("main thread after the others: %d and %d\n", exe_counter, lib_counter);
    return 0;
}
