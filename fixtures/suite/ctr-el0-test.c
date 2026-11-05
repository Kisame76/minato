// Red-first for the CTR_EL0 rewriter site (Formate's ADR-0072 step a needs it): a guest that reads the cache type
// register, CTR_EL0, traps on XNU -- `mrs xN, CTR_EL0` is EXC_BAD_INSTRUCTION at EL0 -- and compiler-rt's
// __clear_cache, which libffi's ffi_prep_closure_loc calls, reads it. The converter's cache_type_read site loads a
// constant instead (0xB445C005: 128-byte lines from the host's hw.cachelinesize, PIPT, IDC=1 and DIC=1), so the read
// never traps and __clear_cache does only dsb+isb, skipping the dc cvau / ic ivau loops that also trap. Setting
// IDC/DIC is honest: Minato never executes guest-generated code (ADR-0001, ADR-0071) and Apple's caches are coherent,
// so no instruction/data cache maintenance is ever needed. Without the site the mrs runs verbatim and the program
// dies with SIGILL; with it, both this read and __clear_cache's are redirected and it prints "ok".
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));  // the instruction XNU traps, rewritten to load the constant
    // The upper 32 bits read as zero, and IDC (bit 28) and DIC (bit 29) must be set so that __clear_cache takes its
    // dsb+isb-only path rather than the cache-line loops.
    if ((ctr >> 32) != 0 || ((ctr >> 28) & 1) == 0 || ((ctr >> 29) & 1) == 0) {
        fprintf(stderr, "CTR_EL0 = %#llx: upper bits set or IDC/DIC clear\n", (unsigned long long)ctr);
        return 1;
    }
    // The real consumer, as libffi's closure preparation runs it: compiler-rt __clear_cache over a buffer. It reads
    // CTR_EL0 itself; with IDC/DIC set it issues no dc/ic, only the barriers, so it returns instead of trapping.
    static char buffer[256];
    __builtin___clear_cache(buffer, buffer + sizeof buffer);
    printf("ok\n");
    return 0;
}
