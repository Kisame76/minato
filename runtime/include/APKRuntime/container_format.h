// Format of a container dylib: the contract between the converter, which writes it, and the
// loader, which reads it (ADR-0008, ADR-0019). Plain C, little-endian, 8-byte aligned.
//
// Layout of a container (addresses relative to its Mach-O header, which dyld places anywhere):
//
//   __TEXT  r-x  Mach-O header and load commands, the section __apk_info with the struct below,
//                the table of x18 islands, the table of runs, the stubs of the rewriter (islands
//                and veneers), then from image_offset on the ELF image: every ELF LOAD segment at
//                image_offset + (p_vaddr - min_vaddr), its first run if that is not writable
//   then one segment per run of the image (ADR-0008 amendment): its 16 KiB pages of one
//   protection in address order, non-writable ones r-x (read-only data included) as __TEXT_1,
//   __TEXT_2, ..., writable ones rw- with their zero-filled parts as __DATA, __DATA_1, ...;
//   one page of runtime slots (struct apk_runtime_slots) that the loader fills in follows the
//   image, in its last run when that is writable, else in a data segment of its own
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
/* 2: the table of x18 islands (ADR-0020 X4); 3: the runs (ADR-0008 amendment); 4: exclusive-window
   islands are resumable (ADR-0020 X4h), so an EXCLUSIVE island's `commit` field changed meaning; 5: the
   APK_ISLAND_SINGLE_NOSAVE kind (ADR-0020 C7), an in-sequence x18 use over a dead register with no red-zone
   save; 6: the (c)-function R-range table (ADR-0020 option c, must-have 6), where a signal finds the guest
   x18 in a register; 7: the converter rewrites mrs CTR_EL0 (ADR-0072), so an app converted before the fix
   must be converted again. The format is unchanged from 6 (the cache_type_read island is self-contained
   code, with no island-table entry), so a version-6 container still loads; the bump only forces the
   reconvert. The converter writes this current version; the loader reads back to
   APK_CONTAINER_OLDEST_VERSION, each at the size its own version had. */
#define APK_CONTAINER_VERSION 9
#define APK_CONTAINER_OLDEST_VERSION 2  /* the loader still reads containers converted before the runs */
/* 9: the flag APK_CONTAINER_MONITOR_INSIDE (ADR-0020 X7 c2): the library holds x18 islands whose own memory accesses lie
   between a guest's load- and store-exclusive, so the loader refuses it on a core that clears the exclusive monitor for
   such an access. The format is unchanged from 8; the bump keeps a runtime that does not check the flag from loading such
   a library, and reconverts the apps converted before it.
   8: the entry-hook table (ADR-0072 decision 3 b): a library that exports ffi_closure_alloc, ffi_closure_free and
   ffi_prep_closure_loc has their first instructions redirected, so that a call by direct branch, which no
   relocation names, reaches the loader's override too. A version 7 or older __apk_info ends before the fields. */
/* From this version a container may hold the (c) R-range table (apk_c_range): the PC ranges where a renamed
   function keeps the guest x18 in a register, so the signal path hands a handler that register's value rather
   than the stale TSD slot 11 (ADR-0020 must-have 6). A container below this version has no ranges; its
   __apk_info ends before the c_ranges fields, so the loader requires only that shorter size for it. */
#define APK_CONTAINER_RANGES_VERSION 6
#define APK_CONTAINER_ENTRY_HOOKS_VERSION 8
/* At most this many entry hooks in a container: the slots the loader fills (apk_runtime_slots.hook). */
#define APK_MAX_ENTRY_HOOKS 7
/* From this version an EXCLUSIVE island is resumable (its `commit` is the preamble length); an older
   container's EXCLUSIVE islands are restartable (`commit` a commit point), so settle() reads them the
   old way and reenter() leaves them alone. */
#define APK_CONTAINER_RESUMABLE_VERSION 4
/* From this version a container may hold an APK_ISLAND_SINGLE_NOSAVE island (ADR-0020 C7): an in-sequence
   x18 use relocated over a dead register, with no red-zone save. A container below this version never holds
   the kind, and the loader's version range keeps older runtimes out: a runtime below this rejects such a
   container, which triggers a reconvert-on-tap (ADR-0020 X4h LaunchDecision). */
#define APK_CONTAINER_RECONSTRUCT_VERSION 5

/// apk_container_info.flags
enum {
    /// Neither PT_INTERP nor DT_NEEDED: the program runs on its own, like the static fixtures.
    APK_CONTAINER_STATIC = 1u << 0,
    /// Some x18 island's own memory accesses lie inside a guest LL/SC window or sequence (ADR-0020 X7 c2): the loader
    /// loads the library only where the core keeps the exclusive monitor across an access to another granule.
    APK_CONTAINER_MONITOR_INSIDE = 1u << 1,
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
    uint64_t writable_start; // ELF addresses from the first writable run to the end of the last one, page aligned;
    uint64_t writable_end;   // version 2 had one writable run, version 3 lists them in `runs`

    // What the rewriter did, for the import log (ADR-0010: every change is counted).
    uint32_t thread_pointer_reads;
    uint32_t thread_pointer_writes;
    uint32_t syscalls;
    uint32_t island_count;  // entries of the island table, one per x18 island

    // Where a signal handler finds out whether a thread stopped inside an x18 island (ADR-0020 X4).
    uint64_t islands;     // from the Mach-O header to the island table (struct apk_island), in __TEXT
    uint64_t stubs;       // from the Mach-O header to the stub area: every island and veneer
    uint64_t stubs_size;  // bytes of the stub area
    // Version 3 (ADR-0008 amendment): the runs of the image, in address order, together from min_vaddr to
    // min_vaddr + image_size. A version 2 container's __apk_info ends before these fields.
    uint64_t runs;       // from the Mach-O header to the table of runs (struct apk_run), in __TEXT
    uint32_t run_count;
    uint32_t reserved;
    // Version 6 (ADR-0020 must-have 6): the (c)-function R-ranges, sorted by `start`, together covering where a
    // renamed function keeps the guest x18 in a register. A container below version 6 ends before these fields.
    uint64_t c_ranges;        // from the Mach-O header to the table of R-ranges (struct apk_c_range), in __TEXT
    uint32_t c_range_count;
    uint32_t reserved2;
    // Version 8 (ADR-0072 decision 3 b): the entry hooks, in __TEXT; 0 of them when the library has none. A container
    // below version 8 ends before these fields.
    uint64_t entry_hooks;  // from the Mach-O header to the table of entry hooks (struct apk_entry_hook)
    uint32_t entry_hook_count;
    uint32_t reserved3;
};

/// apk_run.flags
enum {
    APK_RUN_WRITABLE = 1u << 0,  // rw-; otherwise r-x
};

/// One run of the image: 16 KiB pages of the container with one protection, one Mach-O segment.
struct apk_run {
    uint64_t start;  // ELF addresses, each on a 16 KiB page of the container
    uint64_t end;
    uint32_t flags;
    uint32_t reserved;
};

/// One PC range of a (c)-renamed function where the guest x18 lives in register `reg`, not TSD slot 11 (ADR-0020
/// option c, must-have 6). `start` and `end` are from the Mach-O header, like apk_island.site. A signal whose pc is
/// in [start, end) hands the handler x18 from that register; anywhere else, slot 11 holds it. The table is sorted by
/// `start`, so the runtime binary-searches it.
struct apk_c_range {
    uint32_t start;
    uint32_t end;
    uint8_t reg;
    uint8_t reserved[3];
};

/// apk_island.kind
enum {
    APK_ISLAND_SINGLE = 1,      // one instruction on x18, PC-relative ones included
    APK_ISLAND_BRANCH = 2,      // CBZ, CBNZ, TBZ, TBNZ or a CMPBR compare-and-branch on x18, with its two exits
    APK_ISLAND_EXCLUSIVE = 3,   // a whole LL/SC window with x18 (ADR-0020 X2c)
    APK_ISLAND_JUMP_TABLE = 4,  // BR x18 at the end of a jump table, with its landing pads (X2d)
    APK_ISLAND_INDIRECT = 5,    // BR or BLR x18 that ends no jump table, branching to the guest x18 (X2e)
    APK_ISLAND_PAIR = 7,           // a compare-and-swap pair (CASP) with x18 in a register pair: an even scratch pair stands in
                                   // for (x18, x19) (ADR-0020 X7 c2); value = the even register, base = the odd one
    APK_ISLAND_SINGLE_NOSAVE = 6,  // an in-sequence x18 use over a register proven dead; no red-zone save,
                                   // touches only TSD slot 11 (ADR-0020 C3b, C7). value = the dead register
};
#define APK_ISLAND_NO_REGISTER 0xff  /* apk_island.base of an island with one scratch register */
#define APK_ISLAND_NO_COMMIT 0xff    /* apk_island.commit of an island that changes nothing but pc */

/// One x18 island (ADR-0020 X4); the table is sorted by `stub`. The island saves `value` in the red
/// zone at sp - 128 with its word 0 and `base` at sp - 120 with its word 1. A thread stopped before
/// word k (pc = Mach-O header + stub + 4k) returns to the guest like this: `value` gets back its
/// guest value from sp - 128 once k >= 1, `base` from sp - 120 once k >= 2. For a single instruction
/// or an indirect branch: with k <= commit, or APK_ISLAND_NO_COMMIT, the thread goes back to the site
/// and runs the island again from its start; after the commit it goes on to site + 4 * resume, and
/// while k <= store the island has not stored x18 yet, so the runtime stores the value register as
/// x18 first. From APK_CONTAINER_RESUMABLE_VERSION an APK_ISLAND_EXCLUSIVE window is resumable (ADR-0020
/// X4h): `commit` holds the preamble length and `resume` the window's words, so the thread returns to
/// the exact guest instruction, site + 4 * min(k - commit, resume) -- the window start for a preamble
/// word, the word after the window for the store-back tail; x18 is the value register while
/// commit <= k <= store (the window loads it in the preamble), else slot 11. In an older container an
/// EXCLUSIVE window is restartable and reads
/// like a single instruction above (commit a commit point), which settle() does for its lower version.
struct apk_island {
    uint32_t stub;    // from the Mach-O header to the island's first word
    uint32_t site;    // from the Mach-O header to the rewritten word, which branches to the island
    uint16_t words;   // the island's length, landing pads included
    uint8_t kind;
    uint8_t value;    // the register that stands in for x18
    uint8_t base;     // the second scratch register, or APK_ISLAND_NO_REGISTER
    uint8_t commit;   // completes the guest instruction; for an exclusive window, the preamble length
    uint8_t store;    // the word that stores x18 to the TSD; 0 when there is none
    uint8_t resume;   // words from the site to the word after the commit; for a window, its own words
};

/// One function whose first instruction the converter replaced by a branch to `hook`'s stub (ADR-0072 decision 3 b).
/// The stub loads the address in the container's runtime slot `hook[index]` (the index of the entry in the table) and
/// branches to it. The loader fills the slot with the stub `original`, which runs the displaced instruction and
/// continues at entry + 4, so that the function runs unchanged, or with an override's replacement. `entry` and
/// `original` are from the Mach-O header, like apk_island.site; the hook stub follows the original at +12.
struct apk_entry_hook {
    char name[32];      // the function's name, NUL-terminated
    uint32_t entry;     // the function's first word
    uint32_t original;  // the stub that runs the displaced word and continues at entry + 4
    uint32_t reserved[2];
};

/// In __DATA, written by the loader before any guest code runs.
struct apk_runtime_slots {
    /// Address of the Linux syscall entry. Every svc veneer calls through this slot.
    uint64_t syscall_entry;
    /// Version 8: where the stub of entry hook i branches to; the original stub's address unless an override applies.
    uint64_t hook[APK_MAX_ENTRY_HOOKS];
};

#endif
