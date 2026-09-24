// The places where control passes between host and guest code (ADR-0019, ADR-0025).
//
// apkrt_syscall_entry: called by every svc veneer in converted guest code. The veneer has saved
// x16 and x30; this entry saves all other registers, NZCV and FPSR in a struct apkrt_syscall_frame
// (syscall_frame.h) on the guest's stack, lets the C++ dispatcher work on the frame, and restores
// everything from it. Like the Linux kernel, only x0 changes: guest code may keep values in any
// register across an inline svc. x18 is saved and restored only for completeness; XNU clears it on
// every context switch anyway (F4), and converted code never relies on it.
// The entry and the exit also switch the thread's in_runtime (signals.h, ADR-0025): the entry sets
// it before any C++ runs, the exit clears it and only then looks at the pending signals once more,
// so that none gets lost between the dispatcher's end and guest code. The runtime signal's handler
// knows the words before the set and after the clear by the labels below.
//
// The loader's stubs: libdl's calls from ld-android.so, as syscalls through the entry.
//
// apkrt_call_guest: the runtime's calls into guest code, which run as guest code.
//
// apkrt_save_host / apkrt_return_to_host: _setjmp and _longjmp for the host side of a guest thread
// (threads.cpp), so that SYS_exit gets back to the host's stack. Our own, because libc's longjmp
// is intercepted by AddressSanitizer in the test builds, which then warns about the stack switch.

#include "loader_syscalls.h"

#define FRAME_SIZE 784  // sizeof(struct apkrt_syscall_frame)
#define SIGNALS_MASK 0  // offsetof(ThreadSignals, mask), pending follows it
#define IN_RUNTIME 16   // offsetof(ThreadSignals, in_runtime); ThreadSignals starts a GuestThread

    .text
    .p2align 2
    .globl _apkrt_syscall_entry
_apkrt_syscall_entry:
    hint    #34                     // bti c: called with blr
    sub     sp, sp, #FRAME_SIZE
    // The frame's lowest word first: on a guest stack that is used up, this store faults while the
    // thread still counts as in guest code, a guest fault as at its next push. No later store of
    // the entry can fault: the rest of the frame lies between this word and the veneer's.
    stp     x0, x1, [sp, #0]
    stp     x9, x10, [sp, #72]
    // In the runtime from here on: in_runtime of the calling GuestThread, which the TSD holds under
    // apkrt_guest_thread_key (the load pthread_getspecific does); ~0 while no guest thread exists.
    // Nothing here may touch the flags: NZCV is saved further down.
    adrp    x9, _apkrt_guest_thread_key@PAGE
    ldr     x9, [x9, _apkrt_guest_thread_key@PAGEOFF]
    add     x10, x9, #1
    cbz     x10, 1f
    mrs     x10, TPIDRRO_EL0
    and     x10, x10, #~7
    ldr     x9, [x10, x9, lsl #3]
    cbz     x9, 1f                  // a host thread (tests)
    mov     w10, #1
    strb    w10, [x9, #IN_RUNTIME]
1:
    .globl _apkrt_syscall_entered
_apkrt_syscall_entered:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    str     x8, [sp, #64]
    str     x11, [sp, #88]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x9, nzcv
    mrs     x10, fpsr
    stp     x9, x10, [sp, #256]
    // All 32 SIMD registers in full: the C ABI keeps only the low halves of v8-v15.
    stp     q0, q1, [sp, #272]
    stp     q2, q3, [sp, #304]
    stp     q4, q5, [sp, #336]
    stp     q6, q7, [sp, #368]
    stp     q8, q9, [sp, #400]
    stp     q10, q11, [sp, #432]
    stp     q12, q13, [sp, #464]
    stp     q14, q15, [sp, #496]
    stp     q16, q17, [sp, #528]
    stp     q18, q19, [sp, #560]
    stp     q20, q21, [sp, #592]
    stp     q22, q23, [sp, #624]
    stp     q24, q25, [sp, #656]
    stp     q26, q27, [sp, #688]
    stp     q28, q29, [sp, #720]
    stp     q30, q31, [sp, #752]

    // A frame record over the saved x29/x30, so debuggers and crash logs walk on into the guest.
    add     x29, sp, #232
    mov     x0, sp
    bl      _apkrt_syscall_dispatch     // x0: the thread's ThreadSignals if the exit checks, else 0

    ldp     x9, x10, [sp, #256]
    msr     nzcv, x9
    msr     fpsr, x10
    ldp     q0, q1, [sp, #272]
    ldp     q2, q3, [sp, #304]
    ldp     q4, q5, [sp, #336]
    ldp     q6, q7, [sp, #368]
    ldp     q8, q9, [sp, #400]
    ldp     q10, q11, [sp, #432]
    ldp     q12, q13, [sp, #464]
    ldp     q14, q15, [sp, #496]
    ldp     q16, q17, [sp, #528]
    ldp     q18, q19, [sp, #560]
    ldp     q20, q21, [sp, #592]
    ldp     q22, q23, [sp, #624]
    ldp     q24, q25, [sp, #656]
    ldp     q26, q27, [sp, #688]
    ldp     q28, q29, [sp, #720]
    ldp     q30, q31, [sp, #752]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    // x0 and x1 come last. x16 belongs to the veneer, which restores it, so the check may use it.
    cbz     x0, 2f
    strb    wzr, [x0, #IN_RUNTIME]
    // From here to apkrt_syscall_returned the thread counts as returned from the syscall: a runtime
    // signal delivers on the state after it, from the frame (at the ret: from the registers, the
    // first argument in x16 for a restart).
    .globl _apkrt_syscall_returning
_apkrt_syscall_returning:
    ldp     x1, x16, [x0, #SIGNALS_MASK]    // mask, pending
    bic     x16, x16, x1                    // without touching the guest's flags, restored above
    cbnz    x16, 3f
2:  ldp     x0, x1, [sp, #0]
    ldr     x16, [sp, #248]                 // x[31]: the first argument
    add     sp, sp, #FRAME_SIZE
    .globl _apkrt_syscall_return
_apkrt_syscall_return:
    ret
3:  mov     w1, #1                          // a signal to deliver: back in the runtime first
    strb    w1, [x0, #IN_RUNTIME]
    .globl _apkrt_syscall_returned
_apkrt_syscall_returned:
    mov     x0, sp
    bl      _apkrt_syscall_deliver          // delivers and resumes the guest, never returns
    brk     #1

// libdl's functions for apkrt_loader in ld-android.so (ADR-0025): syscalls of the runtime's own, so
// that the thread is in the runtime for the whole call and what became pending arrives right after
// the return. Each stub puts its number into x8 and calls the entry with the veneer's instructions;
// x8 and x16 are free across a call. The veneer's branch leads to the ret, which the runtime skips
// when it delivers there: the state is then the caller's, after the call.
    .text
    .p2align 2
    .globl _apkrt_loader_stubs
_apkrt_loader_stubs:
.macro LOADER_STUB name, number
    .p2align 2
    .globl _\name
_\name:
    hint    #34                     // bti c: called with blr
    mov     x8, #\number
    b       Lloader_call
.endm

    LOADER_STUB apkrt_loader_dlopen, APKRT_NR_DLOPEN
    LOADER_STUB apkrt_loader_dlsym, APKRT_NR_DLSYM
    LOADER_STUB apkrt_loader_dlclose, APKRT_NR_DLCLOSE
    LOADER_STUB apkrt_loader_dlerror, APKRT_NR_DLERROR
    LOADER_STUB apkrt_loader_dladdr, APKRT_NR_DLADDR

    .p2align 2
Lloader_call:
    stp     x16, x30, [sp, #-16]!
    adrp    x16, _apkrt_syscall_entry@PAGE
    add     x16, x16, _apkrt_syscall_entry@PAGEOFF
    blr     x16
    .globl _apkrt_loader_back
_apkrt_loader_back:
    ldp     x16, x30, [sp], #16
    b       _apkrt_loader_return
    .globl _apkrt_loader_return
_apkrt_loader_return:
    ret
    .globl _apkrt_loader_end
_apkrt_loader_end:

// The calling GuestThread into \reg, 0 on a host thread: what pthread_getspecific finds under
// apkrt_guest_thread_key (~0 while no guest thread exists). Uses x9 and x10.
.macro GUEST_THREAD reg
    mov     \reg, #0
    adrp    x9, _apkrt_guest_thread_key@PAGE
    ldr     x9, [x9, _apkrt_guest_thread_key@PAGEOFF]
    add     x10, x9, #1
    cbz     x10, 1f
    mrs     x10, TPIDRRO_EL0
    and     x10, x10, #~7
    ldr     \reg, [x10, x9, lsl #3]
1:
.endm

// uint64_t apkrt_call_guest(uint64_t function, uint64_t a0, uint64_t a1, uint64_t a2)
//   On a guest thread, the call goes through the runtime signal, as clone's start does: resume()
//   switches to the guest code and clears in_runtime in one step, and delivers what became pending
//   while the runtime worked before the guest code's first word. The guest code returns to 2:, which
//   sets the flag again; a runtime signal treats these words as runtime code. On a host thread it is
//   a plain call.
#define CONTEXT_SIZE 800  // sizeof(apkrt::GuestContext)
    .p2align 2
    .globl _apkrt_call_guest
_apkrt_call_guest:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x19, [sp, #16]
    GUEST_THREAD x19                        // x19: the guest code keeps it
    cbnz    x19, 1f
    mov     x16, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    blr     x16
    b       3f
1:  sub     sp, sp, #CONTEXT_SIZE           // the GuestContext to resume
    stp     x1, x2, [sp, #0]                // x0-x2: the arguments, the other argument registers 0
    stp     x3, xzr, [sp, #16]
    stp     xzr, xzr, [sp, #32]
    stp     xzr, xzr, [sp, #48]
    stp     xzr, xzr, [sp, #64]
    stp     xzr, xzr, [sp, #80]
    stp     xzr, xzr, [sp, #96]
    stp     xzr, xzr, [sp, #112]
    stp     xzr, xzr, [sp, #128]
    stp     xzr, x19, [sp, #144]            // x19-x29 as they are: the guest code keeps them
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    adr     x9, 2f
    add     x10, sp, #CONTEXT_SIZE
    stp     x9, x10, [sp, #240]             // x30: the word after the call; sp: above this context
    stp     x0, xzr, [sp, #256]             // pc: the function; NZCV 0
    mrs     x9, fpsr
    mrs     x10, fpcr
    stp     x9, x10, [sp, #272]
    stp     q0, q1, [sp, #288]              // v8-v15 as they are, the guest code keeps them
    stp     q2, q3, [sp, #320]
    stp     q4, q5, [sp, #352]
    stp     q6, q7, [sp, #384]
    stp     q8, q9, [sp, #416]
    stp     q10, q11, [sp, #448]
    stp     q12, q13, [sp, #480]
    stp     q14, q15, [sp, #512]
    stp     q16, q17, [sp, #544]
    stp     q18, q19, [sp, #576]
    stp     q20, q21, [sp, #608]
    stp     q22, q23, [sp, #640]
    stp     q24, q25, [sp, #672]
    stp     q26, q27, [sp, #704]
    stp     q28, q29, [sp, #736]
    stp     q30, q31, [sp, #768]
    mov     x0, sp
    bl      _apkrt_call_guest_enter         // resumes the context, never returns
2:  mov     w9, #1                          // the guest code returned, with sp above the context
    strb    w9, [x19, #IN_RUNTIME]
3:  ldr     x19, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
    .globl _apkrt_call_guest_end
_apkrt_call_guest_end:

// int apkrt_save_host(uint64_t context[22]): callee-saved registers, sp and lr; returns 0, and 1
// when apkrt_return_to_host comes back to it.
    .p2align 2
    .globl _apkrt_save_host
_apkrt_save_host:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    stp     d8, d9, [x0, #112]
    stp     d10, d11, [x0, #128]
    stp     d12, d13, [x0, #144]
    stp     d14, d15, [x0, #160]
    mov     x0, #0
    ret

// void apkrt_return_to_host(const uint64_t context[22])  -- never returns
    .p2align 2
    .globl _apkrt_return_to_host
_apkrt_return_to_host:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x1, [x0, #96]
    mov     sp, x1
    ldp     d8, d9, [x0, #112]
    ldp     d10, d11, [x0, #128]
    ldp     d12, d13, [x0, #144]
    ldp     d14, d15, [x0, #160]
    mov     x0, #1
    ret
