// Libraries with data inside .text (libdit.S): literal pools, a jump table and real code after a data range,
// with a .symtab (libdit-sym.so) and stripped with its .eh_frame (libdit-strip.so). The run must print what
// Android prints, so the converter neither refuses the libraries for the data's words nor leaves the real svc
// of dit_getpid unrewritten.
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define DECLARE(suffix)                           \
    uint32_t dit_pool##suffix(unsigned i);        \
    uint32_t dit_literal##suffix(void);           \
    int dit_switch##suffix(int i);                \
    long dit_getpid##suffix(void);                \
    long dit_via_adr##suffix(void);               \
    long dit_via_bl##suffix(void);                \
    long dit_via_ptr##suffix(void);               \
    long dit_via_table##suffix(void);             \
    long dit_highword##suffix(void);              \
    long dit_behind##suffix(int i);               \
    long dit_add##suffix(void);                   \
    long dit_ldur##suffix(void);                  \
    int dit_char##suffix(int i);                  \
    long dit_after##suffix(void);                 \
    long dit_nofde##suffix(void);                 \
    long dit_weak_victim##suffix(void);           \
    long dit_noret##suffix(void);                 \
    long dit_weak_x18##suffix(void);
DECLARE(_sym)
DECLARE(_strip)
DECLARE(_rela)
DECLARE(_relr)

#define RUN(suffix)                                                                                   \
    static void run##suffix(const char *name) {                                                       \
        printf("%s pool: %08x %08x %08x %08x\n", name, dit_pool##suffix(0), dit_pool##suffix(1),      \
               dit_pool##suffix(2), dit_pool##suffix(3));                                             \
        printf("%s literal: %08x\n", name, dit_literal##suffix());                                    \
        printf("%s switch: %d %d %d %d\n", name, dit_switch##suffix(0), dit_switch##suffix(1),        \
               dit_switch##suffix(2), dit_switch##suffix(3));                                         \
        printf("%s getpid: %s\n", name, dit_getpid##suffix() == (long)getpid() ? "matches" : "differs"); \
        printf("%s probes: %s %s %ld %c%c %s\n", name, dit_highword##suffix() == (long)getpid() ? "match" : "differ", \
               dit_behind##suffix(0) == (long)getpid() ? "match" : "differ", dit_behind##suffix(1),            \
               dit_char##suffix(0), dit_char##suffix(6),                                                       \
               dit_after##suffix() == (long)getpid() && dit_add##suffix() == (long)getpid() &&                \
                       dit_ldur##suffix() == (long)getpid() && dit_nofde##suffix() == (long)getpid() &&               \
                       dit_weak_victim##suffix() == (long)getpid() &&                                          \
                       dit_noret##suffix() == (long)getpid() && dit_weak_x18##suffix() == (long)getpid()       \
                   ? "match" : "differ");                                                                      \
        printf("%s stubs: %s %s %s %s\n", name, dit_via_adr##suffix() == (long)getpid() ? "match" : "differ", \
               dit_via_bl##suffix() == (long)getpid() ? "match" : "differ",                                  \
               dit_via_ptr##suffix() == (long)getpid() ? "match" : "differ",                                 \
               dit_via_table##suffix() == (long)getpid() ? "match" : "differ");                              \
    }
RUN(_sym)
RUN(_strip)
RUN(_rela)
RUN(_relr)

int main(void) {
    run_sym("sym");
    run_strip("strip");
    run_rela("rela");
    run_relr("relr");
    return 0;
}
