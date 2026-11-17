// x18-sleep-test: a byte of a table read through x18 after the thread slept, in four forms (libx18-sleep.S): x18 as
// the base with a register index, with an immediate, x18 plus 0x10000 into another register as the base, and a leaf
// function that reads through the x18 its caller set before the sleep. Android keeps x18 for the thread, so each reads
// the table; iOS and macOS clear x18 when the thread leaves the CPU, so an x18 use left as it was reads NULL + offset
// (the 0x1087b of the third form is the shape of a crash at that address).
// Expected output: fixtures/expected/x18-sleep-test.out (Android 15's).
#include <stdint.h>
#include <stdio.h>

uint8_t x18_sleep_register(const uint8_t *table, uint64_t index);
uint8_t x18_sleep_immediate(const uint8_t *table);
uint8_t x18_sleep_added(const uint8_t *table);
uint8_t x18_sleep_callee(const uint8_t *table, uint64_t index);

static uint8_t table[0x20000];

int main(void) {
    table[0x7b] = 0x3c;
    table[0x1087b] = 0x5a;
    printf("register: 0x%02x\n", x18_sleep_register(table, 0x1087b));
    printf("immediate: 0x%02x\n", x18_sleep_immediate(table));
    printf("added: 0x%02x\n", x18_sleep_added(table));
    printf("callee: 0x%02x\n", x18_sleep_callee(table, 0x1087b));
    return 0;
}
