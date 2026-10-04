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
