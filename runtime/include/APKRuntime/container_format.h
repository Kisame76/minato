// Format of a container dylib: the contract between the converter, which writes it, and the
// loader, which reads it (ADR-0008, ADR-0019). Plain C, little-endian, 8-byte aligned.
//
// Layout of a container (addresses relative to its Mach-O header, which dyld places anywhere):
//
//   __TEXT  r-x  Mach-O header and load commands, the section __apk_info with the struct below,
//                the stubs of the rewriter (islands and veneers), then from image_offset on the
//                ELF image: every ELF LOAD segment at image_offset + (p_vaddr - min_vaddr), the
//                non-writable ones here
//   __DATA  rw-  the writable ELF segments with their zero-filled parts, then one page of runtime
//                slots (struct apk_runtime_slots) that the loader fills in
//   __LINKEDIT   an empty symbol table and the code signature
//
// All ELF segments keep their distances, so one load bias maps every ELF address:
// runtime address = Mach-O header + image_offset + (ELF address - min_vaddr).
#ifndef APKRUNTIME_CONTAINER_FORMAT_H
#define APKRUNTIME_CONTAINER_FORMAT_H

#include <stdint.h>

#define APK_CONTAINER_SEGMENT "__TEXT"
#define APK_CONTAINER_SECTION "__apk_info"
#define APK_CONTAINER_MAGIC "APKCONT"  /* 8 bytes with the terminating zero */
#define APK_CONTAINER_VERSION 1

/// apk_container_info.flags
enum {
    /// Neither PT_INTERP nor DT_NEEDED: the program runs on its own, like the static fixtures.
    APK_CONTAINER_STATIC = 1u << 0,
};

/// In __TEXT,__apk_info. Read only.
struct apk_container_info {
    char magic[8];
    uint32_t version;
    uint32_t flags;

    uint64_t image_offset;  // from the Mach-O header to the ELF address min_vaddr
    uint64_t image_size;    // bytes from min_vaddr to the end of the last segment, page aligned
    uint64_t min_vaddr;     // lowest ELF address of the image, page aligned (usually 0)
    uint64_t entry;         // e_entry, an ELF address
    uint64_t phdr;          // ELF address of the program headers; 0 when no LOAD segment holds them
    uint32_t phnum;
    uint32_t phentsize;

    uint64_t runtime_slots;  // from the Mach-O header to struct apk_runtime_slots, in __DATA
    uint64_t writable_start; // ELF addresses the loader may write to (relocations), page aligned
    uint64_t writable_end;

    // What the rewriter did, for the import log (ADR-0010: every change is counted).
    uint32_t thread_pointer_reads;
    uint32_t thread_pointer_writes;
    uint32_t syscalls;
    uint32_t reserved;
};

/// In __DATA, written by the loader before any guest code runs.
struct apk_runtime_slots {
    /// Address of the Linux syscall entry. Every svc veneer calls through this slot.
    uint64_t syscall_entry;
    uint64_t reserved[7];
};

#endif
