// libns-dep.so as an app ships it for ns-test (ADR-0040 test 1, ADR-0033 e2a): the same soname as the library in
// ART's folder, another file with another value. An app's class loader must load its own, while ART keeps its own.
int ns_dep_value(void) {
    return 3;
}
