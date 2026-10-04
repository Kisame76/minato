// libdl's functions as syscalls of the runtime's own (ADR-0025): the numbers that the stubs in
// syscall_entry.S put into x8 for apkrt_loader in ld-android.so, above Linux's range. Plain defines,
// for the assembler as well.
#pragma once

#define APKRT_NR_DLOPEN 4096
#define APKRT_NR_DLSYM 4097
#define APKRT_NR_DLCLOSE 4098
#define APKRT_NR_DLERROR 4099
#define APKRT_NR_DLADDR 4100
#define APKRT_NR_DLOPEN_IN 4122  // dlopen in a namespace's search path (ADR-0033 e0), after the others' numbers
// The namespaces of ADR-0040 decision 4 (ADR-0033 e2a).
#define APKRT_NR_CREATE_NAMESPACE 4123
#define APKRT_NR_LINK_NAMESPACES 4124
#define APKRT_NR_EXPORTED_NAMESPACE 4125
#define APKRT_NR_DLOPEN_NAMESPACE 4126
