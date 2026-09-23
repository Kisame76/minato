// libdl-missing.so: libdl-broken.so needs it, but it is never installed next to dl-test.
int missing_dependency(void) {
    return 0;
}
