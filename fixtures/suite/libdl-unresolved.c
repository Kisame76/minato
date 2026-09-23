// libdl-unresolved.so for dl-test: calls a function nobody defines (linked with -z undefs).
int function_nobody_defines(void);

int unresolved_function(void) {
    return function_nobody_defines();
}
