// ns-test's probe of dlsym(RTLD_DEFAULT) (ADR-0033 e2b2): whether a library sees ns_plugin_value, which ns-test opens
// in the default namespace, through the libraries of its own namespace, as linker64's dlsym_linear_lookup searches.
#include <dlfcn.h>

const char *ns_lib_check(void) {
    return dlsym(RTLD_DEFAULT, "ns_plugin_value") != 0 ? "sees ns_plugin_value" : "does not see ns_plugin_value";
}
