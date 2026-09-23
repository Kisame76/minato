// libdl-global.so for dl-test: opened with RTLD_GLOBAL, so dlsym(RTLD_DEFAULT) finds its symbols.
int global_function(void) {
    return 77;
}
