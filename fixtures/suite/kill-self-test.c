// kill-self-test: a process that sends SIGKILL to itself, as Android's Process.killProcess(Process.myPid()) does,
// with which Godot ends (ADR-0028, changed 2026-10-07). A second thread sends it while the main thread waits for that
// thread in pthread_join: nothing runs after it, and the shell shows 137 (128 + SIGKILL). The call is kill() of its
// own pid, or with the argument "tgkill" tgkill() to the main thread, or with "sigqueue" sigqueue() of its own pid,
// which runtime/tests/session_run.py also runs. Expected output: fixtures/expected/kill-self-test.out
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static pid_t main_thread;
static const char *call = "kill";

static void *killer(void *unused) {
    (void)unused;
    printf("kill-self-test: the second thread sends SIGKILL to its own process\n");
    fflush(stdout);
    if (strcmp(call, "tgkill") == 0) {
        syscall(SYS_tgkill, getpid(), main_thread, SIGKILL);
    } else if (strcmp(call, "sigqueue") == 0) {
        union sigval value = {.sival_int = 0};
        sigqueue(getpid(), SIGKILL, value);
    } else {
        kill(getpid(), SIGKILL);
    }
    printf("kill-self-test: the second thread runs on after the kill\n");
    fflush(stdout);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc > 1) call = argv[1];
    main_thread = gettid();
    printf("kill-self-test: the main thread waits for the second thread\n");
    fflush(stdout);
    pthread_t thread;
    pthread_create(&thread, NULL, killer, NULL);
    pthread_join(thread, NULL);
    printf("kill-self-test: the main thread runs on after the kill\n");
    return 0;
}
