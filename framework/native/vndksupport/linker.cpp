// libvndksupport's library loading as on a device without a vendor (sphal) namespace (ADR-0039 b3.2): AOSP's
// android_load_sphal_library then falls back to dlopen, and its unload to dlclose. Minato has no vendor partition.
#include <dlfcn.h>

#include <vndksupport/linker.h>

void* android_load_sphal_library(const char* name, int flag) {
    return dlopen(name, flag);
}

int android_unload_sphal_library(void* handle) {
    return dlclose(handle);
}
