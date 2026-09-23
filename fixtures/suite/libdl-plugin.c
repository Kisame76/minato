// libdl-plugin.so for dl-test: opened at run time, never a DT_NEEDED of the program. It needs
// libdl-helper.so, runs a constructor, and has thread-local storage of its own, which is dynamic
// TLS because the library came after the program started.
#include <stdio.h>

int helper_twice(int value);

__thread int plugin_counter = 3;
int plugin_value = 1234;

__attribute__((constructor)) static void start(void) {
    printf("plugin: constructor ran\n");
}

int plugin_add(int amount) {
    plugin_counter += helper_twice(amount);
    return plugin_counter;
}
