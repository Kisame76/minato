/* libruns-lib.so's sections after its data (runs-test, ADR-0008 amendment): lld places them behind .bss, so the
   library gets a second r-x, an r-- and an rw- LOAD segment after its data, as the Unity test APK v2's libil2cpp.so. */
SECTIONS {
  .text.after : { *(.text.after) }
  .rodata.after : { *(.rodata.after) }
  .data.after : { *(.data.after) }
} INSERT AFTER .bss;
