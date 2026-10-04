// el0-check (ADR-0047 b1): the instructions of the CPU features that ADR-0047 decision 3 reports only after a check
// at EL0 on the device, each tried under a SIGILL handler, with what each did. This probe decided which came in (b2).
// Each line names a feature and its result:
//   - ECV: CNTVCTSS_EL0 and CNTPCTSS_EL0, the self-synchronized counter reads, against CNTVCT_EL0 read around them;
//   - AFP: FPCR's FIZ, AH and NEP bits: kept when written, FIZ flushing a denormal input, and FPCR as the interrupted
//     code had it after a signal handler that cleared it;
//   - SSBS: PSTATE.SSBS through MRS and both forms of MSR, and after a signal handler that changed it;
//   - DIT: the same for PSTATE.DIT, reported since a), the other PSTATE bit EL0 writes, which signal frames keep with
//     SSBS since b2;
//   - WFXT: WFET and WFIT with a deadline that has passed.
// The results differ by device (decision 3 lists which device has what), so tests.txt marks it measures;
// runtime.el0_check_fixture compares the Mac's lines with what its sysctl lets one expect (runtime/tests/el0_check.py).
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

static volatile sig_atomic_t illegal;

// A SIGILL stands for "no such instruction here": the handler steps over the instruction.
static void on_illegal(int signal_number, siginfo_t* info, void* context) {
    (void)signal_number;
    (void)info;
    illegal = 1;
    ((ucontext_t*)context)->uc_mcontext.pc += 4;
}

static uint64_t cntvct(void) {
    uint64_t value;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

// --- ECV ----------------------------------------------------------------------------------------------------

static void ecv(void) {
    uint64_t before = cntvct(), virtual_ss = 0, physical_ss = 0;
    illegal = 0;
    __asm__ volatile("mrs %0, s3_3_c14_c0_6" : "=r"(virtual_ss));  // CNTVCTSS_EL0
    const int virtual_illegal = illegal;
    uint64_t after = cntvct();
    illegal = 0;
    __asm__ volatile("mrs %0, s3_3_c14_c0_5" : "=r"(physical_ss));  // CNTPCTSS_EL0
    const int physical_illegal = illegal;
    printf("ECV: CNTVCTSS_EL0 %s, CNTPCTSS_EL0 %s\n",
           virtual_illegal ? "SIGILL" : before <= virtual_ss && virtual_ss <= after ? "between two CNTVCT_EL0 reads"
                                                                                    : "outside two CNTVCT_EL0 reads",
           physical_illegal ? "SIGILL" : physical_ss != 0 ? "reads" : "reads 0");
}

// --- AFP ----------------------------------------------------------------------------------------------------

static const uint64_t fiz = 1, ah = 2, nep = 4;

static uint64_t read_fpcr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

static void write_fpcr(uint64_t value) {
    __asm__ volatile("msr fpcr, %0\n isb" : : "r"(value) : "memory");
}

static volatile uint64_t fpcr_in_handler;

static void on_user(int signal_number) {
    (void)signal_number;
    fpcr_in_handler = read_fpcr();
    write_fpcr(0);
}

static void afp(void) {
    const uint64_t saved = read_fpcr();
    illegal = 0;
    write_fpcr(saved | fiz | ah | nep);
    const uint64_t written = read_fpcr();
    // With FIZ, a denormal input counts as zero: the smallest denormal float times 2^100 is 0.
    volatile float denormal = 1.4e-45f, factor = 1.2676506e30f;
    volatile float product = denormal * factor;
    raise(SIGUSR1);
    const uint64_t after_handler = read_fpcr();
    write_fpcr(saved);
    volatile float without = denormal * factor;
    printf("AFP: FIZ, AH and NEP %s; FIZ %s; in a signal handler %s; after it cleared them %s\n",
           illegal ? "SIGILL" : (written & (fiz | ah | nep)) == (fiz | ah | nep) ? "kept" : "dropped",
           product == 0.0f && without != 0.0f ? "flushes a denormal input" : "flushes nothing",
           (fpcr_in_handler & (fiz | ah | nep)) == (fiz | ah | nep) ? "still set" : "cleared",
           (after_handler & (fiz | ah | nep)) == (fiz | ah | nep) ? "restored" : "lost");
}

// --- SSBS ---------------------------------------------------------------------------------------------------

static const uint64_t ssbs_bit = 1 << 12;

static uint64_t read_ssbs(void) {
    uint64_t value = 0;
    __asm__ volatile("mrs %0, s3_3_c4_c2_6" : "+r"(value));  // SSBS
    return value;
}

static volatile uint64_t ssbs_in_handler;

static void on_user_ssbs(int signal_number) {
    (void)signal_number;
    ssbs_in_handler = read_ssbs();
    __asm__ volatile(".inst 0xd503403f");  // MSR SSBS, #0
}

static void ssbs(void) {
    illegal = 0;
    const uint64_t first = read_ssbs();
    if (illegal) {
        printf("SSBS: MRS SIGILL\n");
        return;
    }
    __asm__ volatile(".inst 0xd503413f");  // MSR SSBS, #1
    const uint64_t set_immediate = read_ssbs();
    __asm__ volatile("msr s3_3_c4_c2_6, %0" : : "r"(UINT64_C(0)));  // MSR SSBS, Xt
    const uint64_t cleared_register = read_ssbs();
    __asm__ volatile(".inst 0xd503413f");
    signal(SIGUSR1, on_user_ssbs);
    raise(SIGUSR1);
    const uint64_t after_handler = read_ssbs();
    __asm__ volatile("msr s3_3_c4_c2_6, %0" : : "r"(first));
    printf("SSBS: MRS %s; MSR #1 %s; MSR Xt %s; in a signal handler %s; after it cleared it %s\n",
           illegal ? "SIGILL in a later step" : "reads", set_immediate & ssbs_bit ? "sets it" : "does not set it",
           cleared_register & ssbs_bit ? "does not clear it" : "clears it",
           ssbs_in_handler & ssbs_bit ? "set" : "clear", after_handler & ssbs_bit ? "restored" : "lost");
}

// --- DIT ----------------------------------------------------------------------------------------------------

static const uint64_t dit_bit = UINT64_C(1) << 24;

static uint64_t read_dit(void) {
    uint64_t value = 0;
    __asm__ volatile("mrs %0, s3_3_c4_c2_5" : "+r"(value));  // DIT
    return value;
}

static volatile uint64_t dit_in_handler;

static void on_user_dit(int signal_number) {
    (void)signal_number;
    dit_in_handler = read_dit();
    __asm__ volatile(".inst 0xd503405f");  // MSR DIT, #0
}

static void dit(void) {
    illegal = 0;
    const uint64_t first = read_dit();
    if (illegal) {
        printf("DIT: MRS SIGILL\n");
        return;
    }
    __asm__ volatile(".inst 0xd503415f");  // MSR DIT, #1
    const uint64_t set_immediate = read_dit();
    __asm__ volatile("msr s3_3_c4_c2_5, %0" : : "r"(UINT64_C(0)));  // MSR DIT, Xt
    const uint64_t cleared_register = read_dit();
    __asm__ volatile(".inst 0xd503415f");
    signal(SIGUSR1, on_user_dit);
    raise(SIGUSR1);
    const uint64_t after_handler = read_dit();
    __asm__ volatile("msr s3_3_c4_c2_5, %0" : : "r"(first));
    printf("DIT: MRS %s; MSR #1 %s; MSR Xt %s; in a signal handler %s; after it cleared it %s\n",
           illegal ? "SIGILL in a later step" : "reads", set_immediate & dit_bit ? "sets it" : "does not set it",
           cleared_register & dit_bit ? "does not clear it" : "clears it",
           dit_in_handler & dit_bit ? "set" : "clear", after_handler & dit_bit ? "restored" : "lost");
}

// --- WFXT ---------------------------------------------------------------------------------------------------

static void wfxt(void) {
    uint64_t deadline = cntvct();
    illegal = 0;
    __asm__ volatile("mov x0, %0\n .inst 0xd5031000" : : "r"(deadline) : "x0");  // WFET X0
    const int wfet_illegal = illegal;
    illegal = 0;
    __asm__ volatile("mov x0, %0\n .inst 0xd5031020" : : "r"(deadline) : "x0");  // WFIT X0
    printf("WFXT: WFET %s, WFIT %s\n", wfet_illegal ? "SIGILL" : "returns", illegal ? "SIGILL" : "returns");
}

int main(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_illegal;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &action, NULL);
    signal(SIGUSR1, on_user);
    ecv();
    afp();
    ssbs();
    dit();
    wfxt();
    return 0;
}
