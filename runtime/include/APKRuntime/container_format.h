// Format of a container dylib: the contract between the converter, which writes it, and the
// loader, which reads it (ADR-0008, ADR-0019). Plain C, little-endian, 8-byte aligned.
//
// Layout of a container (addresses relative to its Mach-O header, which dyld places anywhere):
//
//   __TEXT  r-x  Mach-O header and load commands, the section __apk_info with the struct below,
//                the table of x18 islands, the stubs of the rewriter (islands and veneers), then
//                from image_offset on the ELF image: every ELF LOAD segment at image_offset +
//                (p_vaddr - min_vaddr), the non-writable ones here
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
#define APK_CONTAINER_VERSION 2  /* 2: the table of x18 islands (ADR-0020 X4) */

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
    uint32_t island_count;  // entries of the island table, one per x18 island

    // Where a signal handler finds out whether a thread stopped inside an x18 island (ADR-0020 X4).
    uint64_t islands;     // from the Mach-O header to the island table (struct apk_island), in __TEXT
    uint64_t stubs;       // from the Mach-O header to the stub area: every island and veneer
    uint64_t stubs_size;  // bytes of the stub area
};

/// apk_island.kind
enum {
    APK_ISLAND_SINGLE = 1,      // one instruction on x18, PC-relative ones included
    APK_ISLAND_BRANCH = 2,      // CBZ, CBNZ, TBZ or TBNZ on x18, with its two exits
    APK_ISLAND_EXCLUSIVE = 3,   // a whole LL/SC window with x18 (ADR-0020 X2c)
    APK_ISLAND_JUMP_TABLE = 4,  // BR x18 at the end of a jump table, with its landing pads (X2d)
};
#define APK_ISLAND_NO_REGISTER 0xff  /* apk_island.base of an island with one scratch register */
#define APK_ISLAND_NO_COMMIT 0xff    /* apk_island.commit of an island that changes nothing but pc */

/// One x18 island (ADR-0020 X4); the table is sorted by `stub`. The island saves `value` in the red
/// zone at sp - 128 with its word 0 and `base` at sp - 120 with its word 1. A thread stopped before
/// word k (pc = Mach-O header + stub + 4k) returns to the guest like this: `value` gets back its
/// guest value from sp - 128 once k >= 1, `base` from sp - 120 once k >= 2. With k <= commit, or
/// APK_ISLAND_NO_COMMIT, the thread goes back to the site and runs the island again from its start;
/// after the commit it goes on to site + 4 * resume, and while k <= store the island has not
/// stored x18 yet, so the runtime stores the value register as x18 first.
struct apk_island {
    uint32_t stub;    // from the Mach-O header to the island's first word
    uint32_t site;    // from the Mach-O header to the rewritten word, which branches to the island
    uint16_t words;   // the island's length, landing pads included
    uint8_t kind;
    uint8_t value;    // the register that stands in for x18
    uint8_t base;     // the second scratch register, or APK_ISLAND_NO_REGISTER
    uint8_t commit;   // the word that completes the guest instruction or window
    uint8_t store;    // the word that stores x18 to the TSD; 0 when there is none
    uint8_t resume;   // words from the site to where the thread goes on after the commit
};

/// In __DATA, written by the loader before any guest code runs.
struct apk_runtime_slots {
    /// Address of the Linux syscall entry. Every svc veneer calls through this slot.
    uint64_t syscall_entry;
    uint64_t reserved[7];
};

#endif
