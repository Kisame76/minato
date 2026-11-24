// fault-site-test's x18 variant: a library from a toolchain that keeps a table's base in x18 (as Unity's older ones
// did) reads byte `index` of it when x18 is 0, the state iOS leaves x18 in. The converter puts both words on x18 into
// islands, so the fault comes from the island and the signal path hands the guest's site back: the load at
// fault_site_read_x18 + 4, with x18 0 as its base. fault_site_read_gx18 sets the guest's x18 to a table's base
// (0x5a5a0000) and reads through another register that is NULL: at its fault the hardware x18 is what the system left,
// the guest's is in TSD slot 11, so the fault-site line's x18 and gx18 differ.

    .text
    .p2align 2
    .globl  fault_site_read_x18
    .type   fault_site_read_x18, %function
// unsigned char fault_site_read_x18(unsigned long index): never returns; the load faults at index.
fault_site_read_x18:
    mov     x18, xzr
    ldrb    w0, [x18, x0]
    ret
    .size   fault_site_read_x18, .-fault_site_read_x18

    .globl  fault_site_read_gx18
    .type   fault_site_read_gx18, %function
// unsigned char fault_site_read_gx18(const unsigned char *base, unsigned long index): never returns with base NULL.
fault_site_read_gx18:
    movz    x18, #0x5a5a, lsl #16
    ldrb    w0, [x0, x1]
    ret
    .size   fault_site_read_gx18, .-fault_site_read_gx18
