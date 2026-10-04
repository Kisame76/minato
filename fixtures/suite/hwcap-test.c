// hwcap-test (ADR-0047 a): what a program learns of the CPU it runs on. It prints getauxval's AT_HWCAP and
// AT_HWCAP2, and what its own ifunc resolver got from the linker, as bionic's linker passes it on arm64: the hwcap
// word with _IFUNC_ARG_HWCAP set, and an __ifunc_arg_t with its size and both words. Then what /proc/cpuinfo shows:
// its number of processors and the first Features line (ADR-0047 c). The values differ from device to device, so
// tests.txt marks it measures; runtime.hwcap_fixture compares them on the Mac with the bits the Mac's sysctl gives
// (runtime/tests/hwcap_check.py).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ifunc.h>

static uint64_t resolver_hwcap;
static uint64_t argument_size;
static uint64_t argument_hwcap;
static uint64_t argument_hwcap2;

static int chosen(void) {
    return 1;
}

// Runs while the linker relocates the program, before main.
static void* resolve(uint64_t hwcap, __ifunc_arg_t* argument) {
    resolver_hwcap = hwcap;
    if (argument != NULL) {
        argument_size = argument->_size;
        argument_hwcap = argument->_hwcap;
        argument_hwcap2 = argument->_hwcap2;
    }
    return (void*)chosen;
}

int probe(void) __attribute__((ifunc("resolve")));

// /proc/cpuinfo's processors, counted by their "processor" lines, and its first Features line, as libraries such as
// cpu_features read them beside getauxval.
static void cpuinfo(void) {
    FILE* file = fopen("/proc/cpuinfo", "r");
    if (file == NULL) {
        printf("/proc/cpuinfo: missing\n");
        return;
    }
    char line[1024];
    char features[1024] = "";
    int processors = 0;
    int features_seen = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "processor\t:", 11) == 0) processors++;
        if (!features_seen && strncmp(line, "Features\t:", 10) == 0) {
            snprintf(features, sizeof features, "%s", line[10] == ' ' ? line + 11 : line + 10);
            features_seen = 1;
        }
    }
    fclose(file);
    printf("/proc/cpuinfo: %d processors, Features: %s\n", processors, features);
}

int main(void) {
    printf("AT_HWCAP 0x%lx\n", getauxval(AT_HWCAP));
    printf("AT_HWCAP2 0x%lx\n", getauxval(AT_HWCAP2));
    printf("ifunc resolver: hwcap 0x%llx, _IFUNC_ARG_HWCAP %s\n",
           (unsigned long long)(resolver_hwcap & ~_IFUNC_ARG_HWCAP),
           (resolver_hwcap & _IFUNC_ARG_HWCAP) != 0 ? "set" : "not set");
    printf("ifunc argument: size %llu, hwcap 0x%llx, hwcap2 0x%llx\n", (unsigned long long)argument_size,
           (unsigned long long)argument_hwcap, (unsigned long long)argument_hwcap2);
    printf("probe: %d\n", probe());
    cpuinfo();
    return 0;
}
