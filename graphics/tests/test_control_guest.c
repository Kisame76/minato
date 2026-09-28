/* The guest side of the tests' way to the host (test_control.h): the table of the front lib "test", bound
   by the runtime's syscall like every front lib's, and one stub, which reaches slot 1 through the entry in
   slot 0. Without graphics-run's provider every command answers 0. */
#include "minato_bind.h"
#include "test_control.h"

MINATO_HIDDEN void *minato_test_table[MINATO_TEST_TABLE_LENGTH];
static const int64_t no_driver[MINATO_TEST_TABLE_LENGTH];

__attribute__((naked)) uint64_t minato_test(uint64_t command, uint64_t a, uint64_t b, uint64_t c) {
    __asm__("adrp x17, minato_test_table\n\t"
            "add x17, x17, :lo12:minato_test_table\n\t"
            "ldr x16, [x17, #8]\n\t"
            "ldr x17, [x17]\n\t"
            "br x17");
}

__attribute__((constructor(101))) static void bind_test(void) {
    minato_bind_host_table("test", minato_test_table, MINATO_TEST_TABLE_LENGTH, MINATO_TEST_TABLE_HASH, no_driver);
}
