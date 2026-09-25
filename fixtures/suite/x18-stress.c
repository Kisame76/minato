// x18-stress: eight threads at once run rounds of x18 islands from x18-stress.S, each with a tag of
// its own in x18, while the main thread sends them SIGUSR1 in turn (ADR-0020 X5). The handler checks
// the frame's x18 against the thread's tag and records where the signal found the thread. Afterwards
// every recorded pc must lie in the code of a loaded object, and the counters must be what the
// rounds give. Every line is fixed by Linux; the numbers of rounds and signals are not printed.
// Expected output: fixtures/expected/x18-stress.out
#define _GNU_SOURCE  // dl_iterate_phdr
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <time.h>

enum {
    threads = 8,
    wanted = 200,     // signals each thread takes inside its loop
    recorded = 4096,  // pcs kept per thread
    chunk = 1000,     // rounds per call of stress_rounds
};

struct stress {
    // What x18-stress.S reads and writes.
    uint64_t x18;       // the thread's x18 between calls: its tag in bits 63:48, a count below
    uint64_t spill;     // where each round stores and loads x18
    uint64_t own;       // the cell of the thread's own LL/SC window
    uint64_t *shared;   // the counter all threads share
    uint64_t cases[4];  // how often each case of the jump table ran
    // Only for the C part.
    uint64_t tag;
    uint64_t rounds;
    int failed;  // a branch that x18 rules out was taken
    volatile sig_atomic_t in_loop;
    atomic_int handled;  // signals taken inside the loop; only the thread's handler writes it
    int wrong_tags;      // frames inside the loop whose x18 did not carry the tag
    uint64_t pcs[recorded];
};
_Static_assert(offsetof(struct stress, cases) == 32, "x18-stress.S knows the layout up to here");

uint64_t stress_rounds(struct stress *state, uint64_t rounds);
extern const char stress_loop_begin[], stress_loop_end[], stress_jump_begin[], stress_jump_end[];
extern const int32_t stress_x18_sites[];
extern const int32_t stress_x18_site_count;

static struct stress states[threads];
static uint64_t shared;
static atomic_int stop;
static __thread struct stress *current;

static void on_usr1(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)info;
    struct stress *state = current;
    if (state == NULL || !state->in_loop) return;
    const ucontext_t *uc = context;
    const uint64_t pc = uc->uc_mcontext.pc;
    const int handled = atomic_load_explicit(&state->handled, memory_order_relaxed);
    if (handled < recorded) state->pcs[handled] = pc;
    // In the jump sequence x18 holds the target instead of the tag.
    const int in_loop = pc >= (uint64_t)stress_loop_begin && pc < (uint64_t)stress_loop_end;
    const int in_jump = pc >= (uint64_t)stress_jump_begin && pc < (uint64_t)stress_jump_end;
    if (in_loop && !in_jump && uc->uc_mcontext.regs[18] >> 48 != state->tag) ++state->wrong_tags;
    atomic_store_explicit(&state->handled, handled + 1, memory_order_relaxed);
}

static void *run(void *argument) {
    struct stress *state = argument;
    current = state;
    while (!atomic_load(&stop)) {
        state->in_loop = 1;
        if (stress_rounds(state, chunk) != 0) state->failed = 1;
        state->in_loop = 0;
        state->rounds += chunk;
    }
    return NULL;
}

struct lookup {
    uint64_t pc;
    int found;
};

static int look_up(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct lookup *lookup = data;
    for (int index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[index];
        if (phdr->p_type != PT_LOAD || (phdr->p_flags & PF_X) == 0) continue;
        const uint64_t start = info->dlpi_addr + phdr->p_vaddr;
        if (lookup->pc >= start && lookup->pc - start < phdr->p_memsz) lookup->found = 1;
    }
    return lookup->found;
}

/// Whether `pc` lies in an executable segment of a loaded object, rather than somewhere else.
static int in_loaded_code(uint64_t pc) {
    struct lookup lookup = {pc, 0};
    dl_iterate_phdr(look_up, &lookup);
    return lookup.found;
}

static int at_x18_instruction(uint64_t pc) {
    for (int index = 0; index < stress_x18_site_count; ++index) {
        if (pc == (uint64_t)stress_loop_begin + (uint64_t)(int64_t)stress_x18_sites[index]) return 1;
    }
    return 0;
}

static int64_t milliseconds_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

int main(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_usr1;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &action, NULL);

    pthread_t ids[threads];
    for (int index = 0; index < threads; ++index) {
        states[index].tag = (uint64_t)index + 1;
        states[index].x18 = states[index].tag << 48;
        states[index].shared = &shared;
        pthread_create(&ids[index], NULL, run, &states[index]);
    }
    // SIGUSR1 to each thread in turn, until each has taken enough inside its loop (at most 60 s).
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const struct timespec pause = {0, 20 * 1000};
    for (int enough = 0; !enough && milliseconds_since(&start) < 60 * 1000;) {
        enough = 1;
        for (int index = 0; index < threads; ++index) {
            if (atomic_load(&states[index].handled) >= wanted) continue;
            enough = 0;
            pthread_kill(ids[index], SIGUSR1);
            nanosleep(&pause, NULL);
        }
    }
    atomic_store(&stop, 1);
    for (int index = 0; index < threads; ++index) pthread_join(ids[index], NULL);

    int took = 1, tags = 1, loaded = 1, at_x18 = 0, branches = 1, rounds = 1;
    uint64_t expected_shared = 0;
    for (int index = 0; index < threads; ++index) {
        const struct stress *state = &states[index];
        const int handled = atomic_load(&state->handled);
        took = took && handled >= wanted;
        tags = tags && state->wrong_tags == 0;
        branches = branches && !state->failed;
        for (int signal = 0; signal < handled && signal < recorded; ++signal) {
            loaded = loaded && in_loaded_code(state->pcs[signal]);
            at_x18 = at_x18 || at_x18_instruction(state->pcs[signal]);
        }
        expected_shared += state->rounds * state->tag;
        // Each round adds 1 to x18 and 1 more in its own window; then the jump table takes bits 2:1
        // of x18, which for round r (from 0) are (r + 1) mod 4.
        const uint64_t x18 = state->tag << 48 | 2 * state->rounds;
        uint64_t cases[4] = {state->rounds / 4};
        for (uint64_t k = 1; k < 4; ++k) cases[k] = state->rounds >= k ? (state->rounds - k) / 4 + 1 : 0;
        rounds = rounds && state->x18 == x18 && state->own == x18 && state->spill == x18 - 1 &&
                 memcmp(cases, state->cases, sizeof cases) == 0;
    }
    printf("%d threads, each with a tag of its own in x18\n", threads);
    printf("every thread took SIGUSR1 at least %d times inside its loop: %s\n", wanted, yes(took));
    printf("x18 in every signal frame inside the loop carried its thread's tag: %s\n", yes(tags));
    printf("every signal found its thread in the code of a loaded object: %s\n", yes(loaded));
    printf("some signal found its thread at an instruction that uses x18: %s\n", yes(at_x18));
    printf("no thread took a branch that its x18 rules out: %s\n", yes(branches));
    printf("the shared counter is each thread's rounds times its tag, summed: %s\n", yes(shared == expected_shared));
    printf("each thread's x18, its cells and its jump table cases are what its rounds give: %s\n", yes(rounds));
    return 0;
}
