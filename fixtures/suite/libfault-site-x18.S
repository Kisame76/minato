// fault-site-test's x18 variant: a library from a toolchain that keeps a table's base in x18 (as Unity's older ones
// did) reads byte `index` of it when x18 is 0, the state iOS leaves x18 in. The converter puts both words on x18 into
// islands, so the fault comes from the island and the signal path hands the guest's site back: the load at
// fault_site_read_x18 + 4, with x18 0 as its base.

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
