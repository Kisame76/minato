// property-test (ADR-0039 decisions 4 and 9): bionic's system property calls on the property area that the
// runtime gives every program at /dev/__properties__: values by name, one through find and read_callback, a
// name that is not there, and the count of all through foreach. Then what android.os.Build reads at its start, as
// libbase's GetProperty reads for Java's SystemProperties.get: find, then read_callback, which gives a long value
// whole. The values are ours: the heap of AOSP's phone-xhdpi-6144-dalvik-heap.mk, Android 15's version as a user
// build, and the device as the host names it, so the emulator prints its own. The model and the device differ by
// host, so only whether they agree is printed; runtime.properties checks them against the host's hw.machine.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/system_properties.h>

static void print_value(void *cookie, const char *name, const char *value, uint32_t serial) {
    (void)serial;
    printf("%s: %s = %s\n", (const char *)cookie, name, value);
}

/// Keeps a value for build_read, up to 511 bytes.
static void keep_value(void *cookie, const char *name, const char *value, uint32_t serial) {
    (void)name;
    (void)serial;
    snprintf((char *)cookie, 512, "%s", value);
}

/// `name`'s value as Build reads it, in `value` of 512 bytes; NULL when the property is not there.
static const char *build_read(const char *name, char *value) {
    const prop_info *info = __system_property_find(name);
    if (info == NULL) return NULL;
    __system_property_read_callback(info, keep_value, value);
    return value;
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

    static const char *const build_names[] = {
        "ro.build.id",
        "ro.build.type",
        "ro.build.tags",
        "ro.build.version.incremental",
        "ro.build.version.release_or_preview_display",
        "ro.build.version.all_codenames",
        "ro.build.version.known_codenames",
        "ro.build.version.min_supported_target_sdk",
        "ro.product.cpu.abilist",
        "ro.product.cpu.abilist32",
        "ro.product.cpu.abilist64",
        "ro.product.manufacturer",
        "ro.product.brand",
        "ro.hardware",
        "ro.sf.lcd_density",
        "ro.opengles.version",
        "ro.build.fingerprint",
        "ro.boot.qemu",
    };
    char build_value[512];
    for (size_t index = 0; index < sizeof build_names / sizeof *build_names; index++) {
        const char *got = build_read(build_names[index], build_value);
        if (got != NULL) {
            printf("build %s: \"%s\"\n", build_names[index], got);
        } else {
            printf("build %s: none\n", build_names[index]);
        }
    }
    char model[512], device[512];
    const char *got_model = build_read("ro.product.model", model);
    const char *got_device = build_read("ro.product.device", device);
    printf("build ro.product.model and ro.product.device: the same, not empty: %s\n",
           got_model != NULL && got_device != NULL && *got_model != '\0' && strcmp(got_model, got_device) == 0 ? "yes"
                                                                                                             : "no");
    return 0;
}
