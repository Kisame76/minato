// libdl-broken.so for dl-test: needs libdl-missing.so, which is not there.
int missing_dependency(void);

int broken_function(void) {
    return missing_dependency();
}
