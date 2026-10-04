// libstress-test: the JNI half of stress-test (ADR-0033 test 4, fixtures/java/stress-test), its parts b and c.
//   b) A SIGUSR2 handler with SA_RESTART, as bionic's signal() installs one, counts the signals each Java thread
//      takes. A native thread sends them one at a time, to part a's threads and to the main thread, and waits until
//      each is handled. While the main thread collects, every other signal goes to it: a handler then runs on the
//      thread that stops the world, and the next one on a thread it stopped.
//   c) Boehm's stop-the-world (stw.h) among six native threads, four of them attached to the VM, as IL2CPP attaches
//      its threads: unattached, one spins and one waits on a semaphore; attached, one waits in Java and three call
//      into Java between their stops and collect in turn.
// What the parts saw, which differs from run to run, goes to stderr in one line, "stress-test stats: ...".
#define _GNU_SOURCE  // gettid
#include <jni.h>
#include <stdlib.h>

#include "stw.h"

enum {
    targets_max = 16,
    signals_minimum = 2000,
    stops_minimum = 300,
    collectors = 3,
    native_threads = collectors + 4,  // the sender, and the Boehm threads that do not collect
    handled_seconds = 10,
    java_list = 64,  // the nodes of Main.boehmWork's list
};

struct target {
    pid_t tid;  // 0 once the thread is gone
    pthread_t thread;
    _Atomic unsigned handled;
};

static JavaVM *vm;
static jclass main_class;
static jmethodID boehm_work, wait_a_while;

static pthread_mutex_t targets_lock = PTHREAD_MUTEX_INITIALIZER;  // the sender holds it from a signal to its handling
static struct target targets[targets_max];                        // the main thread's first
static _Atomic int target_count;
static _Atomic int collecting;  // the main thread is in a collection
static _Atomic unsigned sent, inside_main, inside_others;
static _Atomic int ending;  // part a is over: parts b and c end once they have done their minimum
static _Atomic int finished;
static _Atomic unsigned java_calls, java_wrong;

// --- b) SIGUSR2 -------------------------------------------------------------------------------------------------

static void on_signal(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)info;
    (void)context;
    const int saved = errno;
    const pid_t tid = gettid();
    const int count = atomic_load(&target_count);
    for (int i = 0; i < count; i++) {
        if (targets[i].tid != tid) continue;
        if (atomic_load(&collecting)) atomic_fetch_add(i == 0 ? &inside_main : &inside_others, 1);
        atomic_fetch_add(&targets[i].handled, 1);
        break;
    }
    errno = saved;
}

/// The next signal's target, with targets_lock held: while the main thread collects, every other signal goes to it;
/// else part a's threads take their turns, and the main thread once they are gone.
static struct target *next_target(unsigned number, int *turn) {
    const int count = atomic_load(&target_count);
    if (atomic_load(&collecting) && number % 2 == 0) return &targets[0];
    for (int tries = 1; tries < count; tries++) {
        *turn = *turn % (count - 1) + 1;
        if (targets[*turn].tid != 0) return &targets[*turn];
    }
    return &targets[0];
}

static void *send_signals(void *unused) {
    (void)unused;
    int turn = 0;
    for (unsigned number = 0; !(number >= signals_minimum && atomic_load(&ending)) && !atomic_load(&stw.failed);
         number++) {
        pthread_mutex_lock(&targets_lock);
        struct target *target = next_target(number, &turn);
        const unsigned before = atomic_load(&target->handled);
        const int error = pthread_kill(target->thread, SIGUSR2);
        if (error != 0) stw_fail("signal %u: pthread_kill: %s", number, strerror(error));
        const long deadline = stw_now_ns() + handled_seconds * 1000000000L;
        while (error == 0 && atomic_load(&target->handled) == before) {
            if (stw_now_ns() > deadline) {
                stw_fail("signal %u to thread %d: not handled within %d s", number, target->tid, handled_seconds);
                break;
            }
            const struct timespec pause = {0, 20 * 1000};
            nanosleep(&pause, NULL);
        }
        pthread_mutex_unlock(&targets_lock);
        atomic_store(&sent, number + 1);
        const struct timespec pace = {0, (200 + number % 5 * 100) * 1000L};
        nanosleep(&pace, NULL);
    }
    atomic_fetch_add(&finished, 1);
    return NULL;
}

// --- c) Boehm's stop-the-world ----------------------------------------------------------------------------------

static JNIEnv *attach(void) {
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
        stw_fail("AttachCurrentThread failed");
        return NULL;
    }
    return env;
}

/// True when the call into Java threw, which fails the test.
static bool threw(JNIEnv *env, const char *method) {
    if (!(*env)->ExceptionCheck(env)) return false;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    stw_fail("%s threw", method);
    return true;
}

static void end(JNIEnv *env, struct stw_thread *self) {
    stw_unregister(self);
    if (env != NULL) (*vm)->DetachCurrentThread(vm);
    atomic_fetch_add(&finished, 1);
}

static void *collect(void *unused) {
    (void)unused;
    JNIEnv *env = attach();
    struct stw_thread *self = stw_register();
    for (int n = 0; env != NULL && stw_try_collect(self, stops_minimum, atomic_load(&ending)); n = (n + 1) % 1000) {
        atomic_store(&self->state, stw_in_java);
        const jint sum = (*env)->CallStaticIntMethod(env, main_class, boehm_work, n);
        if (threw(env, "Main.boehmWork")) break;
        atomic_fetch_add(&java_calls, 1);
        if (sum != java_list * n + java_list * (java_list - 1) / 2) atomic_fetch_add(&java_wrong, 1);
        stw_work(self, 2000);
        stw_sleep_us(self, 5000);
    }
    end(env, self);
    return NULL;
}

static void *wait_in_java(void *unused) {
    (void)unused;
    JNIEnv *env = attach();
    struct stw_thread *self = stw_register();
    atomic_store(&self->state, stw_in_java);
    while (env != NULL && !atomic_load(&stw.done)) {
        (*env)->CallStaticVoidMethod(env, main_class, wait_a_while);
        if (threw(env, "Main.waitAWhile")) break;
    }
    end(env, self);
    return NULL;
}

static void *spin(void *unused) {
    (void)unused;
    struct stw_thread *self = stw_register();
    while (!atomic_load(&stw.done)) stw_work(self, 1000);
    end(NULL, self);
    return NULL;
}

static void *wait_for_stops(void *unused) {
    (void)unused;
    struct stw_thread *self = stw_register();
    while (!atomic_load(&stw.done)) stw_wait_for_next_stop(self);
    end(NULL, self);
    return NULL;
}

// --- Main's natives ---------------------------------------------------------------------------------------------

JNIEXPORT void JNICALL Java_Main_init(JNIEnv *env, jclass main) {
    (*env)->GetJavaVM(env, &vm);
    main_class = (*env)->NewGlobalRef(env, main);
    boehm_work = (*env)->GetStaticMethodID(env, main, "boehmWork", "(I)I");
    wait_a_while = (*env)->GetStaticMethodID(env, main, "waitAWhile", "()V");
    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    action.sa_sigaction = on_signal;
    sigaction(SIGUSR2, &action, NULL);
    stw_init();
}

/// Makes the calling thread a target of part b's signals.
JNIEXPORT void JNICALL Java_Main_registerTarget(JNIEnv *env, jclass main) {
    (void)env;
    (void)main;
    pthread_mutex_lock(&targets_lock);
    const int index = atomic_load(&target_count);
    targets[index].tid = gettid();
    targets[index].thread = pthread_self();
    atomic_store(&target_count, index + 1);
    pthread_mutex_unlock(&targets_lock);
}

JNIEXPORT void JNICALL Java_Main_unregisterTarget(JNIEnv *env, jclass main) {
    (void)env;
    (void)main;
    pthread_mutex_lock(&targets_lock);
    const pid_t tid = gettid();
    for (int i = 0; i < atomic_load(&target_count); i++) {
        if (targets[i].tid == tid) targets[i].tid = 0;
    }
    pthread_mutex_unlock(&targets_lock);
}

JNIEXPORT void JNICALL Java_Main_collecting(JNIEnv *env, jclass main, jboolean on) {
    (void)env;
    (void)main;
    atomic_store(&collecting, on);
}

/// Starts part b's sender and part c's threads.
JNIEXPORT void JNICALL Java_Main_startNative(JNIEnv *env, jclass main) {
    (void)env;
    (void)main;
    void *(*const bodies[native_threads])(void *) = {send_signals, spin, wait_for_stops, wait_in_java,
                                                     collect, collect, collect};
    for (int i = 0; i < native_threads; i++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, bodies[i], NULL) != 0) {
            stw_fail("pthread_create failed");
            return;
        }
        pthread_detach(thread);
    }
}

/// Part a is over: parts b and c end once they have done their minimum.
JNIEXPORT void JNICALL Java_Main_stopNative(JNIEnv *env, jclass main) {
    (void)env;
    (void)main;
    atomic_store(&ending, 1);
}

JNIEXPORT jboolean JNICALL Java_Main_nativeDone(JNIEnv *env, jclass main) {
    (void)env;
    (void)main;
    return atomic_load(&finished) == native_threads;
}

/// The first failure, or null.
JNIEXPORT jstring JNICALL Java_Main_failure(JNIEnv *env, jclass main) {
    (void)main;
    return atomic_load(&stw.failed) ? (*env)->NewStringUTF(env, stw.failure) : NULL;
}

/// What parts b and c did, in the order of Main's RESULT_ constants.
JNIEXPORT jintArray JNICALL Java_Main_results(JNIEnv *env, jclass main) {
    (void)main;
    const jint values[] = {
        (jint)atomic_load(&sent),
        (jint)stw.stops,
        (jint)stw.moved,
        (jint)atomic_load(&stw.unblocked),
        (jint)stw_sum_late(false),
        (jint)stw_sum_late(true),
        (jint)stw_found(stw_computing),
        (jint)stw_found(stw_waiting),
        (jint)stw_found(stw_in_java),
        (jint)atomic_load(&java_calls),
        (jint)atomic_load(&java_wrong),
    };
    const jsize count = sizeof values / sizeof values[0];
    jintArray array = (*env)->NewIntArray(env, count);
    (*env)->SetIntArrayRegion(env, array, 0, count, values);
    return array;
}

JNIEXPORT void JNICALL Java_Main_writeStats(JNIEnv *env, jclass main, jint collections) {
    (void)env;
    (void)main;
    dprintf(2,
            "stress-test stats: collections %d, signals %u, inside a collection %u (main %u, others %u), stops %u, "
            "found computing %u sleeping %u waiting %u in java %u, late %u, longest stop %ld us, slowest answer "
            "computing %ld sleeping %ld waiting %ld in java %ld us\n",
            collections, atomic_load(&sent), atomic_load(&inside_main) + atomic_load(&inside_others),
            atomic_load(&inside_main), atomic_load(&inside_others), stw.stops, stw_found(stw_computing),
            stw_found(stw_sleeping), stw_found(stw_waiting), stw_found(stw_in_java), stw_sum_late(false),
            stw.longest_ns / 1000, stw_slowest_ns(stw_computing) / 1000, stw_slowest_ns(stw_sleeping) / 1000,
            stw_slowest_ns(stw_waiting) / 1000, stw_slowest_ns(stw_in_java) / 1000);
}

/// Ends the process at once: after a failure, threads may never leave their handlers.
JNIEXPORT void JNICALL Java_Main_die(JNIEnv *env, jclass main, jint status) {
    (void)env;
    (void)main;
    _exit(status);
}
