// fault-site-test's crash reporter: a second library, loaded with dlopen after the engine's, that installs its SIGSEGV
// handler over the engine's and chains to it, as a crash-reporting SDK installs over an engine's (libfault-site.h).
#include "libfault-site.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

static struct sigaction reporter_old;

static void reporter_handler(int number, siginfo_t *info, void *raw) {
    static const char line[] = "reporter handler: signal 11\n";
    (void)write(STDOUT_FILENO, line, sizeof line - 1);
    if ((reporter_old.sa_flags & SA_SIGINFO) != 0) {
        reporter_old.sa_sigaction(number, info, raw);
    } else if (reporter_old.sa_handler != SIG_DFL && reporter_old.sa_handler != SIG_IGN) {
        reporter_old.sa_handler(number);
    }
}

int fault_site_install_reporter(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = reporter_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGSEGV, &action, &reporter_old);
}
