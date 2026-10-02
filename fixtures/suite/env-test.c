// env-test (ADR-0039 decision 2): prints the environment the program starts with, one entry per line, in its
// order. runtime.app_environment runs it through apk-run with and without --app-environment
// (runtime/tests/environment_run.py).
#include <stdio.h>

extern char **environ;

int main(void) {
    for (char **entry = environ; *entry != NULL; entry++) puts(*entry);
    return 0;
}
