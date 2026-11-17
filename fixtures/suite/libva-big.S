// va-footprint-test's big library: 100 MiB of read-only data, the address space an engine library of that size takes
// when it is mapped (a Unity game's IL2CPP library is of that order). Data, not code, so the converter only copies it.
    .section .rodata.va_big, "a"
    .p2align 12
    .globl  va_big_table
    .type   va_big_table, %object
va_big_table:
    .fill   100 * 1024 * 1024, 1, 0x5a
    .size   va_big_table, .-va_big_table
