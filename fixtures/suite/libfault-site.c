// fault-site-test's library: a byte read through a table's base and an index, as an engine reads a table, kept out of
// line so that the faulting load lies in this library (the x18 variant is in libfault-site-x18.S).

__attribute__((noinline)) unsigned char fault_site_read(const unsigned char *base, unsigned long index) {
    return base[index];
}
