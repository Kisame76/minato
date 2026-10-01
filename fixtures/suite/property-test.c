// property-test (ADR-0039 decisions 4 and 9): bionic's system property calls on the property area that the
// runtime gives every program at /dev/__properties__: values by name, one through find and read_callback, a
// name that is not there, and the count of all through foreach. The values are ours: the heap of AOSP's
// phone-xhdpi-6144-dalvik-heap.mk and Android 15's version, so the emulator prints its own.
#include <stdint.h>
#include <stdio.h>
#include <sys/system_properties.h>

static void print_value(void *cookie, const char *name, const char *value, uint32_t serial) {
    (void)serial;
    printf("%s: %s = %s\n", (const char *)cookie, name, value);
}

static void count(const prop_info *info, void *cookie) {
    (void)info;
    ++*(int *)cookie;
}

int main(void) {
    static const char *const names[] = {
        "dalvik.vm.heapstartsize",       "dalvik.vm.heapgrowthlimit",
        "dalvik.vm.heapsize",            "dalvik.vm.heaptargetutilization",
        "dalvik.vm.heapminfree",         "dalvik.vm.heapmaxfree",
        "ro.build.version.sdk",          "ro.build.version.release",
        "ro.build.version.release_or_codename", "ro.build.version.codename",
        "ro.build.version.security_patch", "ro.debuggable",
    };
    char value[PROP_VALUE_MAX];
    for (size_t index = 0; index < sizeof names / sizeof *names; index++) {
        const int length = __system_property_get(names[index], value);
        printf("get %s: %d \"%s\"\n", names[index], length, value);
    }
    const prop_info *info = __system_property_find("ro.build.version.sdk");
    if (info != NULL) {
        __system_property_read_callback(info, print_value, (void *)"read_callback");
    } else {
        printf("find ro.build.version.sdk: none\n");
    }
    printf("get missing.property: %d\n", __system_property_get("missing.property", value));
    printf("find missing.property: %s\n", __system_property_find("missing.property") == NULL ? "none" : "found");
    int properties = 0;
    __system_property_foreach(count, &properties);
    printf("foreach: %d properties\n", properties);
    return 0;
}
