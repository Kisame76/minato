// Checks how a thread leaves an x18 island when a signal stops it there (ADR-0020 X4): before any
// handler sees the thread, the runtime turns it back into a state the guest really passes through.
// The islands come from a small program with every island form, which the converter converts and
// the loader loads. The stepper single-steps calls into it on the real hardware (stepper.h); clone
// children, guest threads like in the other tests, get signals from another thread and faults.
#include "APKRuntime/apkrt.h"
#include "islands.h"
#include "loader.h"
#include "signals.h"
#include "threads.h"

#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "check.h"
#include "elf_builder.h"
#include "stepper.h"

#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
// probe_clone loads x19-x28 and v8-v15 from probe_marks and stores the child's into
// probe_child_registers (threads_test.cpp checks them; here they only have to exist).
struct ProbeRegisters {
    std::uint64_t x[10];
    __uint128_t q[8];
    std::uint64_t sp;
};
ProbeRegisters probe_marks;
ProbeRegisters probe_child_registers;

long probe_clone(std::uint64_t flags, void *child_stack, int *parent_tid, void *tls, int *child_tid,
                 void (*function)(void *), void *argument);
long probe_syscall(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                   std::uint64_t a3 = 0);
void probe_sigreturn();
}

namespace {

using apkrt::islands::Settled;
using apkrt::testing::Step;

std::filesystem::path scratch;
const char *self_path = nullptr;  // argv[0], to start the test again in a child process

// The program, at its ELF addresses; assembled with the NDK's clang and checked with llvm-objdump.
// Each use of x18 becomes an island of the kind named.
constexpr std::uint32_t code[] = {
    // run(inputs): inputs points at the x18 to use, the counter's address and the jump table index.
    0xf940'0009,  // 0x200 ldr x9, [x0]
    0xf940'0401,  // 0x204 ldr x1, [x0, #8]
    0xf940'080a,  // 0x208 ldr x10, [x0, #16]
    0x9100'0642,  // 0x20c add x2, x18, #1               single, reads
    0xaa09'03f2,  // 0x210 mov x18, x9                   single, writes
    0x9100'0652,  // 0x214 add x18, x18, #1              single, reads and writes
    0x9000'0012,  // 0x218 adrp x18, 0x0                 single, PC-relative
    0xaa09'03f2,  // 0x21c mov x18, x9
    0xb400'0052,  // 0x220 cbz x18, 0x228                conditional branch
    0x9100'0442,  // 0x224 add x2, x2, #1
    0xc85f'7c28,  // 0x228 ldxr x8, [x1]                 LL/SC window, x18 the value it stores
    0x9100'0512,  // 0x22c add x18, x8, #1
    0xc80b'7c32,  // 0x230 stxr w11, x18, [x1]
    0x35ff'ffab,  // 0x234 cbnz w11, 0x228
    0xaa0a'03f2,  // 0x238 mov x18, x10
    0xf100'0e5f,  // 0x23c cmp x18, #3                   single, sets the flags
    0x5400'01c8,  // 0x240 b.hi 0x278
    0x9000'000c,  // 0x244 adrp x12, 0x0
    0x9110'018c,  // 0x248 add x12, x12, #0x400
    0xb8b2'7992,  // 0x24c ldrsw x18, [x12, x18, lsl #2]
    0x8b0c'0252,  // 0x250 add x18, x18, x12
    0xd61f'0240,  // 0x254 br x18                        jump table
    0x9100'2840, 0xd65f'03c0,  // 0x258 add x0, x2, #10; ret
    0x9100'2c40, 0xd65f'03c0,  // 0x260 add x0, x2, #11; ret
    0x9100'3040, 0xd65f'03c0,  // 0x268 add x0, x2, #12; ret
    0x9100'3440, 0xd65f'03c0,  // 0x270 add x0, x2, #13; ret
    0x9280'0000, 0xd65f'03c0,  // 0x278 mov x0, #-1; ret
    // loop(rounds, counter): x18 counts up, and the counter adds each x18.
    0x9100'0652,  // 0x280 add x18, x18, #1              single, reads and writes
    0xb400'00b2,  // 0x284 cbz x18, 0x298                conditional branch, never taken
    0xc85f'7c28,  // 0x288 ldxr x8, [x1]                 LL/SC window that reads x18
    0x8b12'0108,  // 0x28c add x8, x8, x18
    0xc809'7c28,  // 0x290 stxr w9, x8, [x1]
    0x35ff'ffa9,  // 0x294 cbnz w9, 0x288
    0xf100'0400,  // 0x298 subs x0, x0, #1
    0x54ff'ff21,  // 0x29c b.ne 0x280
    0xd65f'03c0,  // 0x2a0 ret
    // fault(address)
    0xf940'0012,  // 0x2a4 ldr x18, [x0]                 single, writes
    0xd65f'03c0,  // 0x2a8 ret
    // forms(inputs): inputs points at the x18 to use, the counter's address, the address of a word
    // that is zero or not, and what the cmpxchg expects in the counter.
    0xf940'0009,  // 0x2ac ldr x9, [x0]
    0xf940'0401,  // 0x2b0 ldr x1, [x0, #8]
    0xf940'080a,  // 0x2b4 ldr x10, [x0, #16]
    0xf940'0c0c,  // 0x2b8 ldr x12, [x0, #24]
    0x1000'0ab2,  // 0x2bc adr x18, 0x410                single, ADR: two words
    0x5800'0a92,  // 0x2c0 ldr x18, 0x410                single, LDR (literal): three words
    0xaa09'03f2,  // 0x2c4 mov x18, x9
    0xc85f'7c28,  // 0x2c8 ldxr x8, [x1]                 LL/SC window that reads and writes x18
    0xb240'0252,  // 0x2cc orr x18, x18, #1
    0x8b12'0108,  // 0x2d0 add x8, x8, x18
    0xc80b'7c28,  // 0x2d4 stxr w11, x8, [x1]
    0x35ff'ff8b,  // 0x2d8 cbnz w11, 0x2c8
    0xc85f'7c28,  // 0x2dc ldxr x8, [x1]                 LL/SC window that a CLREX closes alone
    0x9100'0512,  // 0x2e0 add x18, x8, #1
    0xd503'3f5f,  // 0x2e4 clrex
    0x5280'000d,  // 0x2e8 mov w13, #0
    0xc85f'fc32,  // 0x2ec ldaxr x18, [x1]               cmpxchg: the CLREX right behind the store-exclusive
    0xeb0c'025f,  // 0x2f0 cmp x18, x12
    0x5400'0041,  // 0x2f4 b.ne 0x2fc
    0xc80d'fc29,  // 0x2f8 stlxr w13, x9, [x1]
    0xd503'3f5f,  // 0x2fc clrex
    0x35ff'ff6d,  // 0x300 cbnz w13, 0x2ec
    0xc85f'7d48,  // 0x304 ldxr x8, [x10]                LL/SC window whose cbz can skip its only write of x18
    0xb400'0048,  // 0x308 cbz x8, 0x310
    0xaa08'03f2,  // 0x30c mov x18, x8
    0xc80b'7d48,  // 0x310 stxr w11, x8, [x10]
    0x35ff'ff8b,  // 0x314 cbnz w11, 0x304
    0xaa12'03e0,  // 0x318 mov x0, x18                   single, reads
    0xd65f'03c0,  // 0x31c ret
    // fault_read(marker)
    0xaa00'03e1,  // 0x320 mov x1, x0
    0xf940'0240,  // 0x324 ldr x0, [x18]                 single, reads: x1 holds x18 when it faults
    0xd65f'03c0,  // 0x328 ret
    // fault_window(marker)
    0xc85f'7e48,  // 0x32c ldxr x8, [x18]                LL/SC window that reads x18: x0 holds x18 in it
    0x9100'0508,  // 0x330 add x8, x8, #1
    0xc809'7e48,  // 0x334 stxr w9, x8, [x18]
    0x35ff'ffa9,  // 0x338 cbnz w9, 0x32c
    0xd65f'03c0,  // 0x33c ret
    // indirect(inputs): inputs[0] = the BLR target, inputs[1] = the BR target (both runtime addresses).
    // Saves its own LR, since BLR clobbers it (as it must), and restores it before the tail BR, so the
    // tail's RET returns to indirect()'s caller. The BLR must return to the word after it (0x358).
    0xa9bf'7bfd,  // 0x340 stp x29, x30, [sp, #-16]!     save FP and LR
    0xf940'0009,  // 0x344 ldr x9, [x0]                  the blr target (helper)
    0xf940'040a,  // 0x348 ldr x10, [x0, #8]             the br target (tail)
    0xd280'0002,  // 0x34c mov x2, #0
    0xaa09'03f2,  // 0x350 mov x18, x9                   single, writes
    0xd63f'0240,  // 0x354 blr x18                       indirect call island (X2e)
    0x9100'0c42,  // 0x358 add x2, x2, #3                reached only if the blr returned here
    0xa8c1'7bfd,  // 0x35c ldp x29, x30, [sp], #16       restore FP and LR
    0xaa0a'03f2,  // 0x360 mov x18, x10                  single, writes
    0xd61f'0240,  // 0x364 br x18                        indirect branch island (X2e)
    0xd280'0142,  // 0x368 helper: mov x2, #10
    0xd65f'03c0,  // 0x36c ret                           returns to the call site 0x358
    0xaa02'03e0,  // 0x370 tail: mov x0, x2
    0xd65f'03c0,  // 0x374 ret                           returns x2 to indirect()'s caller
    // fault_window_deep(cell): x0 is a mapped address, x18 is unmapped. The load-exclusive succeeds,
    // then a guest load from x18 faults -- a window that relocates only because X4h allows a memory
    // access inside it; the fault must reach the handler at that load, not at the window's start.
    0xc85f'7c08,  // 0x378 ldxr x8, [x0]
    0xf940'0249,  // 0x37c ldr x9, [x18]                 faults: x18 unmapped, a guest load (class 2)
    0xc80a'7c08,  // 0x380 stxr w10, x8, [x0]
    0x35ff'ffaa,  // 0x384 cbnz w10, 0x378
    0xd65f'03c0,  // 0x388 ret
    // backedge(cell, rounds): does `rounds` passes, each incrementing x18, with a back-edge to its own
    // load-exclusive (ADR-0020 X4h, finding A). x18 is written, so the island loads it in the preamble;
    // the store-back to slot 11 runs only when the loop ends, so during the loop slot 11 is stale and a
    // signal at the load-exclusive must take x18 from the value register, not slot 11.
    0xc85f'7c08,  // 0x38c ldxr x8, [x0]                 the window's load-exclusive (word P)
    0x9100'0652,  // 0x390 add x18, x18, #1
    0xf100'0421,  // 0x394 subs x1, x1, #1
    0x54ff'ffa1,  // 0x398 b.ne 0x38c                    back-edge to the load-exclusive while rounds != 0
    0xc809'7c08,  // 0x39c stxr w9, x8, [x0]             one store to end the window (its result is ignored)
    0xd65f'03c0,  // 0x3a0 ret
    // exit_loop(inputs): inputs[0] = x18 (the new value), inputs[1] = the cell, inputs[2] = the expected value. A
    // strong compare-and-swap with the new value in x18 and a branch out of the loop when the cell differs
    // (ADR-0020 X7): the island runs that branch through an exit pad. Returns the cell's old value.
    0xf940'0012,  // 0x3a4 ldr x18, [x0]                 single, writes
    0xf940'0801,  // 0x3a8 ldr x1, [x0, #16]
    0xf940'0400,  // 0x3ac ldr x0, [x0, #8]
    0xc85f'fc08,  // 0x3b0 ldaxr x8, [x0]                the window's load-exclusive
    0xeb01'011f,  // 0x3b4 cmp x8, x1
    0x5400'0061,  // 0x3b8 b.ne 0x3c4                    leaves the window
    0xc809'fc12,  // 0x3bc stlxr w9, x18, [x0]           x18 read here
    0x35ff'ff89,  // 0x3c0 cbnz w9, 0x3b0
    0xaa08'03e0,  // 0x3c4 mov x0, x8
    0xd65f'03c0,  // 0x3c8 ret
    // entry_loop(inputs): inputs[0] = x18, [1] = the cell, [2] = the expected value, [3] = a flag. The same compare-and-swap
    // with a branch from outside into the window (ADR-0020 X7): with the flag set the cbnz jumps onto the store-exclusive,
    // which runs without its load-exclusive, fails and sends the loop round to the load-exclusive. Returns the old value.
    0xf940'0012,  // 0x3cc ldr x18, [x0]                 single, writes
    0xf940'0801,  // 0x3d0 ldr x1, [x0, #16]
    0xf940'0c02,  // 0x3d4 ldr x2, [x0, #24]
    0xf940'0400,  // 0x3d8 ldr x0, [x0, #8]
    0xaa01'03e8,  // 0x3dc mov x8, x1
    0xb500'0082,  // 0x3e0 cbnz x2, 0x3f0                the branch from outside, onto the store-exclusive
    0xc85f'fc08,  // 0x3e4 ldaxr x8, [x0]                the window's load-exclusive
    0xeb01'011f,  // 0x3e8 cmp x8, x1
    0x5400'0061,  // 0x3ec b.ne 0x3f8                    leaves the window
    0xc809'fc12,  // 0x3f0 stlxr w9, x18, [x0]           x18 read here, the window's entry
    0x35ff'ff89,  // 0x3f4 cbnz w9, 0x3e4
    0xaa08'03e0,  // 0x3f8 mov x0, x8
    0xd65f'03c0,  // 0x3fc ret
};
// reg_loop(inputs): inputs[0] = x18, [1] = the cell, [2] = the expected value, [3] = a flag. The same compare-and-swap with a
// register branch in its window (ADR-0020 X7): with the flag set the cbz does not skip the `br x6`, which leaves the window
// for the label after it, through an exit pad that restores the value register and runs the br. Returns the old value.
constexpr std::uint32_t reg_code[] = {
    0xf940'0012,  // 0x420 ldr x18, [x0]                 single, writes
    0xf940'0801,  // 0x424 ldr x1, [x0, #16]
    0xf940'0c02,  // 0x428 ldr x2, [x0, #24]
    0xf940'0400,  // 0x42c ldr x0, [x0, #8]
    0x1000'0106,  // 0x430 adr x6, 0x450                 where the br goes
    0xc85f'fc08,  // 0x434 ldaxr x8, [x0]                the window's load-exclusive
    0xeb01'011f,  // 0x438 cmp x8, x1
    0x5400'00a1,  // 0x43c b.ne 0x450                    leaves the window
    0xb400'0042,  // 0x440 cbz x2, 0x448                 skips the register branch unless the flag is set
    0xd61f'00c0,  // 0x444 br x6                         leaves the window through an exit pad
    0xc809'fc12,  // 0x448 stlxr w9, x18, [x0]           x18 read here
    0x35ff'ff49,  // 0x44c cbnz w9, 0x434
    0xaa08'03e0,  // 0x450 mov x0, x8
    0xd65f'03c0,  // 0x454 ret
    // write_loop(inputs): inputs[0] = x18, [1] = the cell, [2] = a bound. A loop that WRITES x18 and leaves it by a branch
    // (ADR-0020 X7): while the cell is below the bound it sets x18 to twice the cell and increments the cell; at the bound
    // the b.hs leaves with x18 as it was. Returns x18, which shows whether the island stored it back on each way out.
    0xf940'0012,  // 0x458 ldr x18, [x0]                 single, writes
    0xf940'0401,  // 0x45c ldr x1, [x0, #8]
    0xf940'0802,  // 0x460 ldr x2, [x0, #16]
    0xc85f'fc23,  // 0x464 ldaxr x3, [x1]                the window's load-exclusive
    0xeb02'007f,  // 0x468 cmp x3, x2
    0x5400'00a2,  // 0x46c b.hs 0x480                    leaves the window
    0x8b03'0072,  // 0x470 add x18, x3, x3               writes x18, so a retry gives the same
    0x9100'0464,  // 0x474 add x4, x3, #1
    0xc805'fc24,  // 0x478 stlxr w5, x4, [x1]
    0x35ff'ff45,  // 0x47c cbnz w5, 0x464
    0xaa12'03e0,  // 0x480 mov x0, x18
    0xd65f'03c0,  // 0x484 ret
    // pair_loop(inputs): inputs[0], [1] = the expected pair (x18, x19), [2] = a pair of words in memory, [3], [4] = the new
    // pair. A compare-and-swap pair with x18 in its destination pair (ADR-0020 X7): the island runs it on an even scratch
    // pair and writes x18 and x19 back. Returns x18 + (x19 << 8) as the CASP left them: the memory's old pair.
    0xaa13'03e9,  // 0x488 mov x9, x19                   x19 is callee-saved: kept here
    0xf940'0012,  // 0x48c ldr x18, [x0]                 single, writes
    0xf940'0413,  // 0x490 ldr x19, [x0, #8]
    0xf940'0804,  // 0x494 ldr x4, [x0, #16]
    0xf940'0c02,  // 0x498 ldr x2, [x0, #24]
    0xf940'1003,  // 0x49c ldr x3, [x0, #32]
    0x4832'7c82,  // 0x4a0 casp x18, x19, x2, x3, [x4]   the pair island
    0x8b13'2240,  // 0x4a4 add x0, x18, x19, lsl #8
    0xaa09'03f3,  // 0x4a8 mov x19, x9
    0xd65f'03c0,  // 0x4ac ret
};
constexpr std::int32_t jump_table[] = {0x258 - 0x400, 0x260 - 0x400, 0x268 - 0x400, 0x270 - 0x400};
constexpr std::uint64_t literal = 0x1122'3344'5566'7788;  // what the LDR (literal) of forms() loads
constexpr std::uint64_t run_at = 0x200, run_end = 0x280, window_at = 0x228, loop_at = 0x280, fault_at = 0x2a4,
                        forms_at = 0x2ac, forms_end = 0x320, cmpxchg_at = 0x2ec, fault_read_at = 0x320,
                        fault_window_at = 0x32c, indirect_at = 0x340, helper_at = 0x368, tail_at = 0x370,
                        fault_window_deep_at = 0x378, backedge_at = 0x38c, exit_loop_at = 0x3a4, exit_loop_end = 0x3cc,
                        exit_window_at = 0x3b0, entry_loop_at = 0x3cc, entry_loop_end = 0x400, entry_window_at = 0x3e4, table_at = 0x400, literal_at = 0x410,
                        reg_loop_at = 0x420, reg_loop_end = 0x458, reg_window_at = 0x434, write_loop_at = 0x458, write_loop_end = 0x488,
                        write_window_at = 0x464, pair_loop_at = 0x488, pair_loop_end = 0x4b0, program_end = 0x4b0;

// The jump table island (rewrite.cpp, emit_jump_table): 13 words up to its BR, a trap of 3, then a
// landing pad of 4 words for each word from the lowest target on, whose fourth word never runs.
constexpr unsigned jump_words_to_br = 13, jump_pads_at = 16, jump_pad_words = 4, jump_pad_words_run = 3;
constexpr std::uint64_t lowest_target = 0x258;

std::vector<std::byte> program_container() {
    namespace elf = apk::testing;
    elf::ElfBuilder builder;
    builder.put<std::uint64_t>(24, run_at);  // e_entry
    builder.bytes.resize(program_end);
    builder.segments({{elf::pt_load, elf::r | elf::x, 0, 0, program_end, program_end, 0x4000}});
    std::memcpy(builder.bytes.data() + run_at, code, sizeof code);
    std::memcpy(builder.bytes.data() + table_at, jump_table, sizeof jump_table);
    std::memcpy(builder.bytes.data() + literal_at, &literal, sizeof literal);
    std::memcpy(builder.bytes.data() + reg_loop_at, reg_code, sizeof reg_code);
    builder.sections({
        {".text", elf::sht_progbits, elf::shf_alloc | elf::shf_execinstr, run_at, run_at, sizeof code},
        {".rodata", elf::sht_progbits, elf::shf_alloc, table_at, table_at, 0x418 - table_at},
        {".text.reg", elf::sht_progbits, elf::shf_alloc | elf::shf_execinstr, reg_loop_at, reg_loop_at, sizeof reg_code},
    });
    return apk::container::convert(builder.bytes, {.install_name = "@rpath/islands-program.dylib"}).bytes;
}

/// The program, converted, signed ad hoc and loaded once; the loader registers its island table.
struct Program {
    std::uint64_t header;
    std::uint64_t image;  // host address of ELF address 0
    std::span<const apk_island> islands;

    /// The island whose site is the word at ELF address `address`.
    const apk_island &island_at(std::uint64_t address) const {
        for (const auto &island : islands) {
            if (header + island.site == image + address) return island;
        }
        throw std::runtime_error(std::format("no island at {:#x}", address));
    }
};

const Program &program() {
    static const Program loaded = [] {
        auto container = program_container();
        apk::codesign::sign(container, {.identifier = "islands-program"});
        const auto path = scratch / "islands-program.dylib";
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(container.data()), static_cast<std::streamsize>(container.size()));
        }
        const auto image = apkrt::loader::load(path.string());
        const auto header = reinterpret_cast<std::uint64_t>(image.header);
        const auto *table = reinterpret_cast<const apk_island *>(header + image.info->islands);
        return Program{header, image.bias, {table, image.info->island_count}};
    }();
    return loaded;
}

apkrt::GuestContext context_of(const Step &step) {
    apkrt::GuestContext context{};
    for (int index = 0; index < 29; ++index) context.x[index] = step.state.__x[index];
    context.x[29] = arm_thread_state64_get_fp(step.state);
    context.x[30] = arm_thread_state64_get_lr(step.state);
    context.sp = arm_thread_state64_get_sp(step.state);
    context.pc = arm_thread_state64_get_pc(step.state);
    context.nzcv = step.state.__cpsr & 0xf000'0000;
    return context;
}

/// What a comparison leaves out.
struct Ignored {
    std::uint32_t registers = 0;
    bool flags = false;
};

/// How `settled` differs from the state `real` recorded: every register, sp, pc, the flags and x18,
/// but what `ignored` names. Empty when it does not.
std::string difference(const Settled &settled, const Step &real, Ignored ignored) {
    const auto expected = context_of(real);
    for (int index = 0; index < 31; ++index) {
        if ((ignored.registers >> index) & 1) continue;
        if (settled.context.x[index] != expected.x[index]) {
            return std::format("x{} is {:#x}, the guest had {:#x}", index, settled.context.x[index], expected.x[index]);
        }
    }
    if (settled.context.sp != expected.sp) return std::format("sp is {:#x}, the guest had {:#x}", settled.context.sp, expected.sp);
    if (settled.context.pc != expected.pc) return std::format("pc is {:#x}, the guest had {:#x}", settled.context.pc, expected.pc);
    if (!ignored.flags && settled.context.nzcv != expected.nzcv) {
        return std::format("the flags are {:#x}, the guest had {:#x}", settled.context.nzcv, expected.nzcv);
    }
    if (settled.x18 != real.x18) return std::format("x18 is {:#x}, the guest had {:#x}", settled.x18, real.x18);
    return {};
}

/// Whether the word `word` of `island` can run: all but the jump table's trap and the landing pads
/// of words no case of the table starts at.
bool runs(const apk_island &island, unsigned word) {
    if (island.kind != APK_ISLAND_JUMP_TABLE) return true;
    if (word < jump_words_to_br) return true;
    if (word < jump_pads_at || (word - jump_pads_at) % jump_pad_words >= jump_pad_words_run) return false;
    const auto target = lowest_target + 4 * ((word - jump_pads_at) / jump_pad_words);
    for (const auto entry : jump_table) {
        if (table_at + static_cast<std::uint64_t>(std::int64_t{entry}) == target) return true;
    }
    return false;
}

using Covered = std::set<std::pair<const apk_island *, unsigned>>;

/// The free run of an LL/SC window (stepper.h): from its load-exclusive to the word after its
/// store-exclusive, from visit `after_visits` of the load-exclusive on. The island body begins at the
/// preamble (window.commit, ADR-0020 X4h) with the load-exclusive; the store-exclusive follows it.
apkrt::testing::FreeRun free_run(const apk_island &window, unsigned after_visits) {
    CHECK(window.kind == APK_ISLAND_EXCLUSIVE);
    const auto stub = program().header + window.stub;
    const auto word_at = [&](unsigned w) { return *reinterpret_cast<const std::uint32_t *>(stub + 4 * w); };
    const unsigned load = window.commit;  // the preamble length: the first body word, the load-exclusive
    CHECK((word_at(load) & 0x3f40'0000) == 0x0840'0000);
    unsigned store = load;
    while (store < window.words && (word_at(store) & 0x3f40'0000) != 0x0800'0000) ++store;
    CHECK(store < window.words);
    return {stub + 4 * load, stub + 4 * (store + 1u), after_visits};
}

/// Single-steps one call of the program's function at `function_at`, run again when the trace has a
/// gap; `prepare` sets up the inputs before each try and returns the argument. leave() runs at each
/// step while the thread waits there. Wherever the thread was inside an island, its result must
/// equal the state the guest really passed through in the direction the trace shows (see below).
/// Adds each island word stepped to `covered`, counts the comparisons in `compared`, and returns
/// what the call returned.
std::uint64_t step_and_compare(std::uint64_t function_at, const std::function<std::uint64_t()> &prepare,
                               const std::vector<apkrt::testing::FreeRun> &free_runs, Covered &covered,
                               int &compared) {
    const auto &loaded = program();
    const auto function = reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(loaded.image + function_at);
    std::vector<std::pair<bool, Settled>> left;  // leave() at each step, while the thread waited there
    std::vector<Step> steps;
    std::uint64_t result = 0;
    const auto lost_steps_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int attempt = 1;; ++attempt) {
        const auto argument = prepare();
        left.clear();
        try {
            steps = apkrt::testing::step_through(function, argument, free_runs, result, [&](const Step &step) {
                auto context = context_of(step);
                auto x18 = step.x18;
                const bool inside = apkrt::islands::leave(context, x18);
                left.push_back({inside, {context, x18}});
            });
            break;
        } catch (const apkrt::testing::LostSteps &) {
            // The trace has a gap: run the call again. Under load gaps come in bursts (stepper.h) that can outlast a
            // short budget, so each try waits a little longer first (capped at 50 ms), up to a deadline rather than a
            // fixed try count -- it keeps retrying as long as the burst lasts, enough to ride out heavy parallel load.
            if (std::chrono::steady_clock::now() >= lost_steps_deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt < 50 ? attempt : 50));
        }
    }
    for (std::size_t at = 0; at < steps.size(); ++at) {
        const auto pc = arm_thread_state64_get_pc(steps[at].state);
        const auto *container = apkrt::islands::container_at(pc);
        const auto *island = container != nullptr ? apkrt::islands::island_at(*container, pc) : nullptr;
        if (island == nullptr) {
            CHECK(!left[at].first);
            continue;
        }
        const auto word = static_cast<unsigned>((pc - (loaded.header + island->stub)) / 4);
        covered.insert({island, word});
        const auto site = loaded.header + island->site;
        const auto location = std::source_location::current();
        if (island->kind == APK_ISLAND_EXCLUSIVE) {
            // A relocated window is resumable (ADR-0020 X4h): the thread returns to the exact guest
            // instruction, site + 4 * min(word - preamble, words). The trace holds the whole guest
            // state at the window's entry (site) and its exit (after the window); a body word in
            // between maps to an original instruction the island replaced, which the trace never ran,
            // so only its reconstructed pc is checked there.
            const unsigned preamble = island->commit, body = island->resume;
            auto guest = word < preamble ? 0u : std::min<unsigned>(word - preamble, body);
            auto guest_pc = site + 4 * guest;
            // The pads behind the tail (ADR-0020 X7), walked here from the island's words as the converter lays them:
            // an exit pad (ldur, b) takes the thread to the target of the branch that left the window, whose state is
            // the guest's at that target, found like an exit's; an entry pad (the preamble, b into the body) puts it at
            // the window word the entry replaced, whose state is the guest's when it took the branch from outside,
            // found like an entry's.
            bool earlier_state = guest == 0;  // the state to compare with comes from before this word, else after it
            const unsigned tail = island->base != APK_ISLAND_NO_REGISTER ? 6 : 2;
            for (unsigned pad = preamble + body + tail; pad < island->words;) {
                const auto *words = reinterpret_cast<const std::uint32_t *>(loaded.header + island->stub);
                const bool read_exit = (words[pad] & 0xffe0'0c00) == 0xf840'0000;  // ldur
                const bool write_exit = (words[pad] & 0xffff'ffe0) == 0xd53b'd060;  // mrs: the store-back of x18 first
                const bool exit_pad = read_exit || write_exit;
                const unsigned length = read_exit ? 2 : write_exit ? 6 : preamble + 1;
                if (word >= pad && word < pad + length) {
                    const auto branch = words[pad + length - 1];
                    const auto displacement = static_cast<std::int64_t>(static_cast<std::int32_t>(branch << 6) >> 6);
                    const auto target = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(loaded.header + island->stub + 4 * (pad + length - 1)) + 4 * displacement);
                    if (exit_pad) {
                        const bool register_branch = (branch & 0xffff'fc1f) == 0xd61f'0000 || (branch & 0xffff'fc1f) == 0xd65f'0000;
                        guest_pc = register_branch ? context_of(steps[at]).x[(branch >> 5) & 0x1f] : target;
                        guest = body;
                        earlier_state = false;
                    } else {
                        guest_pc = site + 4 * ((target - (loaded.header + island->stub)) / 4 - preamble);
                        guest = 0;
                        earlier_state = true;
                    }
                    break;
                }
                pad += length;
            }
            if (!left[at].first) {
                apk::testing::fail(location, "leave() did not see an island at ", std::format("{:#x}", pc).c_str());
            } else if (left[at].second.context.pc != guest_pc) {
                const auto detail = std::format("site {:#x}, word {}: pc {:#x}, expected {:#x}", island->site, word,
                                                left[at].second.context.pc, guest_pc);
                apk::testing::fail(location, "a resumable window went to the wrong guest pc: ", detail.c_str());
            } else if (guest == 0 || guest == body) {
                std::size_t real = steps.size();
                if (earlier_state) {
                    for (auto earlier = at; earlier-- > 0 && real == steps.size();) {
                        if (arm_thread_state64_get_pc(steps[earlier].state) == guest_pc) real = earlier;
                    }
                } else {
                    for (auto later = at + 1; later < steps.size() && real == steps.size(); ++later) {
                        if (arm_thread_state64_get_pc(steps[later].state) == guest_pc) real = later;
                    }
                }
                if (real == steps.size()) {
                    apk::testing::fail(location, "no state to compare with at ", std::format("{:#x}", guest_pc).c_str());
                } else if (const auto different = difference(left[at].second, steps[real], Ignored{}); !different.empty()) {
                    const auto detail = std::format("site {:#x}, word {} ({}): {}", island->site, word,
                                                    guest == 0 ? "entry" : "exit", different);
                    apk::testing::fail(location, "left a window to a state the guest never had: ", detail.c_str());
                }
            }
            ++compared;
            continue;
        }
        const bool forward = island->commit != APK_ISLAND_NO_COMMIT && word > island->commit;
        std::size_t real = steps.size();
        if (forward) {
            const auto after = site + 4 * std::uint64_t{island->resume};
            for (auto later = at + 1; later < steps.size() && real == steps.size(); ++later) {
                if (arm_thread_state64_get_pc(steps[later].state) == after) real = later;
            }
        } else {
            for (auto earlier = at; earlier-- > 0 && real == steps.size();) {
                if (arm_thread_state64_get_pc(steps[earlier].state) == site) real = earlier;
            }
        }
        if (!left[at].first) {
            apk::testing::fail(location, "leave() did not see an island at ", std::format("{:#x}", pc).c_str());
        } else if (real == steps.size()) {
            apk::testing::fail(location, "the trace has no state to compare with at ", std::format("{:#x}", pc).c_str());
        } else if (const auto different = difference(left[at].second, steps[real], Ignored{}); !different.empty()) {
            const auto detail = std::format("site {:#x}, word {} ({}): {}", island->site, word,
                                            forward ? "forward" : "back", different);
            apk::testing::fail(location, "left an island to a state the guest never had: ", detail.c_str());
        }
        ++compared;
    }
    return result;
}

// ADR-0020 X4: wherever a signal stops a thread inside an island, the state the runtime computes
// equals the state the guest really passed through. A single instruction or an indirect branch goes
// back to the site before its commit and on after it. A relocated window is resumable (X4h): it
// returns to the exact guest instruction; the trace holds the whole guest state at the window's entry
// and exit (compared exactly here), and a body word in between is checked by its pc. All registers,
// sp, pc, flags and x18 count.
// ADR-0020 must-have 6: a container's (c) R-ranges are the table for a version-6 container and empty for an older
// one (whose __apk_info ends before the fields), so a signal in an old container reads x18 from slot 11 as before.
TEST(c_ranges_are_the_table_for_a_new_container_and_empty_for_an_old_one) {
    alignas(apk_container_info) std::byte buffer[sizeof(apk_container_info) + 2 * sizeof(apk_c_range)] = {};
    auto *info = reinterpret_cast<apk_container_info *>(buffer);
    auto *table = reinterpret_cast<apk_c_range *>(buffer + sizeof(apk_container_info));
    table[0] = {0x100, 0x200, 9, {}};
    table[1] = {0x300, 0x340, 10, {}};
    info->c_ranges = sizeof(apk_container_info);
    info->c_range_count = 2;
    const apkrt::islands::Container container{
        .header = reinterpret_cast<std::uint64_t>(buffer), .info = info, .name = {}, .next = nullptr};
    info->version = APK_CONTAINER_RANGES_VERSION - 1;  // an old container: no ranges
    CHECK(apkrt::islands::c_ranges(container).empty());
    info->version = APK_CONTAINER_RANGES_VERSION;  // a version-6 container: the table
    const auto ranges = apkrt::islands::c_ranges(container);
    CHECK(ranges.size() == 2);
    if (ranges.size() == 2) {
        CHECK(ranges[0].start == 0x100u && ranges[0].end == 0x200u && ranges[0].reg == 9);
        CHECK(ranges[1].start == 0x300u && ranges[1].reg == 10);
    }
}

// ADR-0020 must-have 6 (option c): c_range_register finds, for a pc inside a (c) function's R-range of a loaded
// container, the register that holds the guest x18 there, and nullopt anywhere else. The signal path calls it so a
// handler reads x18 from that register instead of the stale TSD slot 11. add() keeps the container for the life of
// the process, so the backing is static. The two ranges are sorted by start, as the converter writes them.
TEST(c_range_register_finds_the_register_inside_a_range_and_nothing_outside) {
    alignas(apk_container_info) static std::byte buffer[sizeof(apk_container_info) + 2 * sizeof(apk_c_range)] = {};
    auto *info = reinterpret_cast<apk_container_info *>(buffer);
    auto *table = reinterpret_cast<apk_c_range *>(buffer + sizeof(apk_container_info));
    table[0] = {0x100, 0x200, 11, {}};
    table[1] = {0x300, 0x340, 13, {}};
    info->version = APK_CONTAINER_RANGES_VERSION;
    info->c_ranges = sizeof(apk_container_info);
    info->c_range_count = 2;
    apkrt::islands::add(buffer, *info, "c-range-lookup");
    const auto header = reinterpret_cast<std::uint64_t>(buffer);
    using apkrt::islands::c_range_register;
    CHECK(c_range_register(header + 0x100) == 11u);  // start is inclusive
    CHECK(c_range_register(header + 0x1ff) == 11u);  // the last byte of the range
    CHECK(c_range_register(header + 0x300) == 13u);
    CHECK(c_range_register(header + 0x33f) == 13u);
    CHECK(!c_range_register(header + 0x0f0).has_value());  // before the first range
    CHECK(!c_range_register(header + 0x200).has_value());  // end is exclusive
    CHECK(!c_range_register(header + 0x250).has_value());  // between the two ranges
    CHECK(!c_range_register(header + 0x340).has_value());  // past the last range
}

TEST(every_word_of_every_island_leaves_to_a_real_guest_state) {
    const auto &loaded = program();
    Covered covered;
    int compared = 0;
    std::uint64_t counter = 0, counter_before = 0;

    // range_covered(from, to): true when every runnable island word whose site lies in [from, to) is already in
    // `covered`. Each retry section below keeps running passes until this holds for its own range, giving up only
    // after kStaleLimit passes in a row that step no new word (a deadline by progress, not a fixed count): a step lost
    // under single-stepping load is retried as long as the passes keep making progress (ADR-0020 X7).
    // A pass that steps no new word is "stale"; this many stale passes in a row (no progress) end a retry
    // section, so a genuinely unsteppable word is reported while load, which still makes progress, is ridden out.
    constexpr int kStaleLimit = 20;
    const auto range_covered = [&](std::uint64_t from, std::uint64_t to) {
        for (const auto &island : loaded.islands) {
            const auto site = loaded.header + island.site - loaded.image;
            if (!(site >= from && site < to)) continue;
            for (unsigned word = 0; word < island.words; ++word) {
                if (runs(island, word) && !covered.contains({&island, word})) return false;
            }
        }
        return true;
    };

    // run(): single instructions, a conditional branch taken both ways, an LL/SC window with x18 as
    // the value it stores, and the jump table with each index. Pass 1: the window runs free. Pass 2:
    // its first visit is stepped up to the store-exclusive, which then fails; the loop runs the
    // window again, free.
    for (const std::uint64_t x18 : {std::uint64_t{0}, std::uint64_t{5}}) {
        for (std::uint64_t index = 0; index < std::size(jump_table); ++index) {
            for (const unsigned stepped_visits : {0u, 1u}) {
                const std::uint64_t inputs[] = {x18, reinterpret_cast<std::uint64_t>(&counter), index};
                const auto result = step_and_compare(
                    run_at,
                    [&] {
                        counter_before = counter;
                        return reinterpret_cast<std::uint64_t>(inputs);
                    },
                    {free_run(loaded.island_at(window_at), stepped_visits)}, covered, compared);
                CHECK(result == 1 + (x18 != 0 ? 1u : 0u) + 10 + index);
                CHECK(counter == counter_before + 1);  // the window completed exactly once
            }
        }
    }

    // forms(): ADR and LDR (literal); an LL/SC window that loads x18 before it and stores it after
    // (its write of x18 gives the same when the window runs again, like real code's would);
    // one that a CLREX closes alone; a cmpxchg with its CLREX right behind the store-exclusive, which
    // swaps or does not; and one whose cbz can skip its only write of x18, which it does when the
    // word is zero. Each window with a store-exclusive runs free or is stepped first, as above.
    std::vector<apkrt::testing::FreeRun> free_runs;
    for (const unsigned stepped_visits : {0u, 1u}) {
        free_runs.clear();
        for (const std::uint64_t at : {std::uint64_t{0x2c8}, cmpxchg_at, std::uint64_t{0x304}}) {
            free_runs.push_back(free_run(loaded.island_at(at), stepped_visits));
        }
        for (const std::uint64_t x18 : {std::uint64_t{2}, std::uint64_t{7}}) {
            for (const std::uint64_t word_value : {std::uint64_t{0}, std::uint64_t{0x55}}) {
                for (const bool swaps : {false, true}) {
                    std::uint64_t word = 0;
                    std::uint64_t inputs[4] = {};
                    const auto result = step_and_compare(
                        forms_at,
                        [&] {
                            counter_before = counter;
                            word = word_value;
                            const auto sum = counter_before + (x18 | 1);  // the counter after the first window
                            inputs[0] = x18;
                            inputs[1] = reinterpret_cast<std::uint64_t>(&counter);
                            inputs[2] = reinterpret_cast<std::uint64_t>(&word);
                            inputs[3] = swaps ? sum : sum + 1;
                            return reinterpret_cast<std::uint64_t>(inputs);
                        },
                        free_runs, covered, compared);
                    const auto sum = counter_before + (x18 | 1);
                    CHECK(result == (word_value != 0 ? word_value : sum));
                    CHECK(counter == (swaps ? x18 : sum));
                    CHECK(word == word_value);
                }
            }
        }
    }

    // exit_loop(): the compare-and-swap that stores x18 and the one whose branch leaves the window, through an exit pad
    // (ADR-0020 X7), every word of the island stepped, the pad's two words included: wherever the thread stops, leave()
    // must give the state the guest passes through, and in a pad the leaving branch's target. Up to six passes,
    // stopping as soon as range_covered says the exit_loop range was fully stepped (a step may be lost under load).
    for (int stale = 0; stale < kStaleLimit;) {
        const std::size_t covered_before = covered.size();
        for (const unsigned stepped_visits : {0u, 1u}) {
            for (const bool differs : {false, true}) {
                if (differs && stepped_visits == 1) continue;  // no store-exclusive on that path: one pass is enough
                std::uint64_t cell_value = 0;
                std::uint64_t inputs[3] = {};
                const auto result = step_and_compare(
                    exit_loop_at,
                    [&] {
                        cell_value = 41;
                        inputs[0] = 77;  // the new value, in x18
                        inputs[1] = reinterpret_cast<std::uint64_t>(&cell_value);
                        inputs[2] = differs ? 40 : 41;
                        return reinterpret_cast<std::uint64_t>(inputs);
                    },
                    // Single-stepping clears the monitor, so a pass that stores runs its window free (see free_run);
                    // the pass that leaves before its store-exclusive has none to fail and is stepped whole.
                    differs ? std::vector<apkrt::testing::FreeRun>{}
                            : std::vector{free_run(loaded.island_at(exit_window_at), stepped_visits)},
                    covered, compared);
                CHECK(result == 41);
                CHECK(cell_value == (differs ? 41u : 77u));
            }
        }
        if (range_covered(exit_loop_at, exit_loop_end)) break;
        stale = covered.size() == covered_before ? stale + 1 : 0;
    }

    // entry_loop(): the same loop entered from outside at its store-exclusive (ADR-0020 X7), every word of the island and of
    // its entry pad stepped: wherever the thread stops in the pad, leave() gives the guest at the branch from outside.
    for (int stale = 0; stale < kStaleLimit;) {
        const std::size_t covered_before = covered.size();
        for (const bool entered : {false, true}) {
            for (const bool differs : {false, true}) {
                for (const unsigned stepped_visits : {0u, 1u}) {
                    if (differs && stepped_visits == 1) continue;
                    std::uint64_t cell_value = 0;
                    std::uint64_t inputs[4] = {};
                    const auto result = step_and_compare(
                        entry_loop_at,
                        [&] {
                            cell_value = 41;
                            inputs[0] = 77;
                            inputs[1] = reinterpret_cast<std::uint64_t>(&cell_value);
                            inputs[2] = differs ? 40 : 41;
                            inputs[3] = entered ? 1 : 0;
                            return reinterpret_cast<std::uint64_t>(inputs);
                        },
                        differs ? std::vector<apkrt::testing::FreeRun>{}
                                : std::vector{free_run(loaded.island_at(entry_window_at), stepped_visits)},
                        covered, compared);
                    CHECK(result == 41);
                    CHECK(cell_value == (differs ? 41u : 77u));
                }
            }
        }
        if (range_covered(entry_loop_at, entry_loop_end)) break;
        stale = covered.size() == covered_before ? stale + 1 : 0;
    }

    // reg_loop(): a register branch in the window, which leaves it through an exit pad, run as the guest wrote it (ADR-0020
    // X7): every word stepped, the pad's two included; wherever the thread stops, its pc is where x6 says.
    for (int stale = 0; stale < kStaleLimit;) {
        const std::size_t covered_before = covered.size();
        for (const bool leaves : {false, true}) {
            for (const bool differs : {false, true}) {
                for (const unsigned stepped_visits : {0u, 1u}) {
                    const bool stores = !leaves && !differs;  // only that pass reaches the store-exclusive
                    if (!stores && stepped_visits == 1) continue;
                    std::uint64_t cell_value = 0;
                    std::uint64_t inputs[4] = {};
                    const auto result = step_and_compare(
                        reg_loop_at,
                        [&] {
                            cell_value = 41;
                            inputs[0] = 77;
                            inputs[1] = reinterpret_cast<std::uint64_t>(&cell_value);
                            inputs[2] = differs ? 40 : 41;
                            inputs[3] = leaves ? 1 : 0;
                            return reinterpret_cast<std::uint64_t>(inputs);
                        },
                        stores ? std::vector{free_run(loaded.island_at(reg_window_at), stepped_visits)}
                               : std::vector<apkrt::testing::FreeRun>{},
                        covered, compared);
                    CHECK(result == 41);
                    CHECK(cell_value == (stores ? 77u : 41u));
                }
            }
        }
        if (range_covered(reg_loop_at, reg_loop_end)) break;
        stale = covered.size() == covered_before ? stale + 1 : 0;
    }

    // write_loop(): a window that writes x18 and leaves by a branch (ADR-0020 X7), every word stepped, the exit pad's six
    // included (its store-back of x18 first, then the restores and the branch): wherever the thread stops, x18 and the
    // other registers are the guest's.
    for (int stale = 0; stale < kStaleLimit;) {
        const std::size_t covered_before = covered.size();
        for (const bool leaves : {false, true}) {
            for (const unsigned stepped_visits : {0u, 1u}) {
                if (leaves && stepped_visits == 1) continue;
                std::uint64_t cell_value = 0;
                std::uint64_t inputs[3] = {};
                const auto result = step_and_compare(
                    write_loop_at,
                    [&] {
                        cell_value = leaves ? 11 : 3;
                        inputs[0] = 100;
                        inputs[1] = reinterpret_cast<std::uint64_t>(&cell_value);
                        inputs[2] = 10;  // the bound
                        return reinterpret_cast<std::uint64_t>(inputs);
                    },
                    leaves ? std::vector<apkrt::testing::FreeRun>{}
                           : std::vector{free_run(loaded.island_at(write_window_at), stepped_visits)},
                    covered, compared);
                CHECK(result == (leaves ? 100u : 6u));
                CHECK(cell_value == (leaves ? 11u : 4u));
            }
        }
        if (range_covered(write_loop_at, write_loop_end)) break;
        stale = covered.size() == covered_before ? stale + 1 : 0;
    }

    // pair_loop(): a CASP with x18 in its destination pair (ADR-0020 X7), every island word stepped: before the CASP the thread
    // goes back to the site, after it on to the next word with x18 and x19 as the CASP left them, wherever it stops.
    alignas(16) std::uint64_t pair_memory[2];
    for (int stale = 0; stale < kStaleLimit;) {
        const std::size_t covered_before = covered.size();
        for (const bool matches : {true, false}) {
            std::uint64_t inputs[5] = {};
            const auto result = step_and_compare(
                pair_loop_at,
                [&] {
                    pair_memory[0] = 7;
                    pair_memory[1] = 9;
                    inputs[0] = matches ? 7 : 1;  // x18: the expected low half
                    inputs[1] = matches ? 9 : 2;  // x19: the expected high half
                    inputs[2] = reinterpret_cast<std::uint64_t>(pair_memory);
                    inputs[3] = 100;
                    inputs[4] = 200;
                    return reinterpret_cast<std::uint64_t>(inputs);
                },
                {}, covered, compared);
            CHECK(result == 7 + (9u << 8));  // the memory's old pair, whether it was swapped
            CHECK(pair_memory[0] == (matches ? 100u : 7u) && pair_memory[1] == (matches ? 200u : 9u));
        }
        if (range_covered(pair_loop_at, pair_loop_end)) break;
        stale = covered.size() == covered_before ? stale + 1 : 0;
    }

    CHECK(compared > 0);
    // Every word of every island of run() and forms() that can run was stepped.
    for (const auto &island : loaded.islands) {
        const auto site = loaded.header + island.site - loaded.image;
        if (!(site >= run_at && site < run_end) && !(site >= forms_at && site < forms_end) &&
            !(site >= exit_loop_at && site < exit_loop_end) && !(site >= entry_loop_at && site < entry_loop_end) &&
            !(site >= reg_loop_at && site < reg_loop_end) && !(site >= write_loop_at && site < write_loop_end) &&
            !(site >= pair_loop_at && site < pair_loop_end)) {
            continue;
        }
        for (unsigned word = 0; word < island.words; ++word) {
            if (runs(island, word) && !covered.contains({&island, word})) {
                const auto detail = std::format("site {:#x}, word {}", island.site, word);
                apk::testing::fail(std::source_location::current(), "never stepped: ", detail.c_str());
            }
        }
    }
}

// ADR-0020 X4h: settle() maps every island word of a relocated window to the exact guest
// instruction, the scratch registers and x18 reconstructed, from the island fields alone. A window
// with a guest store inside, stopped just after it, resumes past the store so it is never run twice.
TEST(a_resumable_exclusive_island_maps_every_word_to_the_guest_instruction) {
    // A write window that reads x18 first: a 5-word body (ldxr, add reading x18, str inside the
    // window, add writing x18, stxr), a preamble of 5 (save value, save base, mrs, bic, ldr), the x18
    // store-back at word 12.
    constexpr std::uint8_t preamble = 5, body = 5, value = 9, base = 10;
    apk_island island{};
    island.stub = 0x1000;
    island.site = 0x400;  // the window's guest address
    island.words = preamble + body + 3 /* mrs, bic, str */ + 2 /* restore value, base */ + 1 /* branch */;
    island.kind = APK_ISLAND_EXCLUSIVE;
    island.value = value;
    island.base = base;
    island.commit = preamble;
    island.store = preamble + body + 2;  // 12: the str that writes x18 back to slot 11
    island.resume = body;
    constexpr std::uint64_t header = 0x1'0000'0000;
    constexpr std::uint64_t worked = 0xbeef, saved_value = 0xcafe, saved_base = 0xf00d, slot11 = 0x1818,
                            had_base = 0x9999;
    for (unsigned k = 0; k < island.words; ++k) {
        apkrt::GuestContext context{};
        context.pc = header + island.stub + 4 * k;
        context.x[value] = worked;   // the island's x18 stand-in
        context.x[base] = had_base;  // the island's TSD base scratch
        const auto settled = apkrt::islands::settle(island, header, context, saved_value, saved_base, slot11, true);
        const std::uint64_t guest = k < preamble ? 0 : std::min<std::uint64_t>(k - preamble, body);
        CHECK(settled.context.pc == header + island.site + 4 * guest);
        CHECK(settled.context.x[value] == (k >= 1 ? saved_value : worked));
        CHECK(settled.context.x[base] == (k >= 2 ? saved_base : had_base));
        const bool x18_in_value = k >= preamble && k <= island.store;  // value holds x18 from the first body word
        CHECK(settled.x18 == (x18_in_value ? worked : slot11));
    }
    // The inner store is body word 2 (island word preamble + 2). Stopped at the next word, the guest
    // pc is past the store (site + 4 * 3), so resuming never runs the store a second time (ADR item 2).
    apkrt::GuestContext after_store{};
    after_store.pc = header + island.stub + 4 * (preamble + 3);
    after_store.x[value] = worked;
    const auto settled = apkrt::islands::settle(island, header, after_store, saved_value, saved_base, slot11, true);
    CHECK(settled.context.pc == header + island.site + 4 * 3);
}

// ADR-0020 X7: a thread stopped in an exit pad of a window that only reads x18 continues at the target of the branch
// that left the window, which the pad's own B names, and has its value register back; a word of the window body or of
// the tail keeps the map above. The pads sit behind the tail: two words each, the restore and the B.
TEST(an_exit_pad_leaves_to_the_target_of_the_branch_that_left_the_window) {
    constexpr std::uint8_t preamble = 4, body = 5, value = 9;  // save value; mrs, bic, ldr; ldaxr, cmp, b.ne, stlxr, cbnz
    constexpr unsigned tail = 2, pads = 2;
    std::vector<std::uint32_t> stub(preamble + body + tail + 2 * pads, 0xd503'201f);
    const auto base_address = reinterpret_cast<std::uint64_t>(stub.data());
    const std::uint64_t targets[] = {base_address + 0x1000, base_address - 0x400};  // two exits, forward and back
    apk_island island{};
    island.stub = 0x1000;
    island.site = 0x400;
    island.words = static_cast<std::uint16_t>(stub.size());
    island.kind = APK_ISLAND_EXCLUSIVE;
    island.value = value;
    island.base = APK_ISLAND_NO_REGISTER;
    island.commit = preamble;
    island.store = 0;
    island.resume = body;
    const std::uint64_t header = reinterpret_cast<std::uint64_t>(stub.data()) - island.stub;
    for (unsigned pad = 0; pad < pads; pad++) {
        stub[preamble + body + tail + 2 * pad] = 0xf858'03e9;  // ldur x9, [sp, #-128]: the exit pad's restore
        const unsigned at_word = preamble + body + tail + 2 * pad + 1;
        const std::uint64_t at = header + island.stub + 4 * at_word;
        const std::int64_t distance = static_cast<std::int64_t>(targets[pad]) - static_cast<std::int64_t>(at);
        stub[at_word] = 0x1400'0000u | (static_cast<std::uint32_t>(distance / 4) & 0x03ff'ffffu);  // b target
    }
    constexpr std::uint64_t worked = 0xbeef, saved_value = 0xcafe, slot11 = 0x1818;
    for (unsigned k = 0; k < island.words; ++k) {
        apkrt::GuestContext context{};
        context.pc = header + island.stub + 4 * k;
        context.x[value] = worked;
        const auto settled = apkrt::islands::settle(island, header, context, saved_value, 0, slot11, true);
        const unsigned first_pad = preamble + body + tail;
        if (k >= first_pad) {
            CHECK(settled.context.pc == targets[(k - first_pad) / 2]);
        } else {
            const std::uint64_t guest = k < preamble ? 0 : std::min<std::uint64_t>(k - preamble, body);
            CHECK(settled.context.pc == 0 + header + island.site + 4 * guest);
        }
        CHECK(settled.context.x[value] == (k >= 1 ? saved_value : worked));
        CHECK(settled.x18 == slot11);  // the window only reads x18: slot 11 holds it throughout
    }
}

// (b) ADR-0020 C7: a read no-save island loads the guest x18 into a dead value register, runs the use,
// and branches back -- no red-zone save, no store-back. settle() maps each word to the site (before the
// commit) or site + 4 (after), x18 stays slot 11 (a read never writes it), and the dead value register is
// NEVER restored from the red zone (the island saved nothing there). The end-to-end no-fault leave() is
// covered by the step-8 fixtures; here the settle() map is checked directly.
TEST(a_no_save_read_island_maps_every_word_and_restores_no_register) {
    constexpr std::uint8_t value = 5;
    apk_island island{};
    island.stub = 0x1000;
    island.site = 0x400;
    island.words = 5;  // mrs, bic, ldr, the use, the branch back
    island.kind = APK_ISLAND_SINGLE_NOSAVE;
    island.value = value;
    island.base = APK_ISLAND_NO_REGISTER;
    island.commit = 3;  // the use is word 3 (mrs 0, bic 1, ldr 2)
    island.store = 0;   // a read never stores x18 back
    island.resume = 1;
    constexpr std::uint64_t header = 0x1'0000'0000;
    constexpr std::uint64_t worked = 0xbeef, saved_value = 0xcafe, saved_base = 0xf00d, slot11 = 0x1818;
    for (unsigned k = 0; k < island.words; ++k) {
        apkrt::GuestContext context{};
        context.pc = header + island.stub + 4 * k;
        context.x[value] = worked;  // the island's x18 stand-in, a register proven dead after the use
        const auto settled = apkrt::islands::settle(island, header, context, saved_value, saved_base, slot11, false);
        const bool back = k <= island.commit;  // before the commit it restarts at the site; after, it goes on
        CHECK(settled.context.pc == header + island.site + (back ? 0 : 4));
        CHECK(settled.x18 == slot11);                     // a read leaves slot 11 the guest value throughout
        CHECK(settled.context.x[value] == worked);        // the dead register is not restored from the red zone
    }
}

// (b) ADR-0020 C7: a write no-save island adds a store-back through a second dead register. From just after
// the use (the commit) until the str runs (the store word) x18 lives in the value register; before the
// commit and after the store it is slot 11. Neither dead register is restored from the red zone.
TEST(a_no_save_write_island_holds_x18_in_the_value_register_until_the_store_back) {
    constexpr std::uint8_t value = 5, base = 6;
    apk_island island{};
    island.stub = 0x1000;
    island.site = 0x400;
    island.words = 8;  // mrs, bic, ldr, the use, mrs, bic, str, the branch back
    island.kind = APK_ISLAND_SINGLE_NOSAVE;
    island.value = value;
    island.base = base;
    island.commit = 3;  // the use is word 3
    island.store = 6;   // str x_value, [x_base, #slot11] is word 6
    island.resume = 1;
    constexpr std::uint64_t header = 0x1'0000'0000;
    constexpr std::uint64_t worked = 0xbeef, saved_value = 0xcafe, saved_base = 0xf00d, slot11 = 0x1818,
                            had_base = 0x9999;
    for (unsigned k = 0; k < island.words; ++k) {
        apkrt::GuestContext context{};
        context.pc = header + island.stub + 4 * k;
        context.x[value] = worked;    // the new x18, once the use has run
        context.x[base] = had_base;   // the store-back base scratch
        const auto settled = apkrt::islands::settle(island, header, context, saved_value, saved_base, slot11, false);
        const bool back = k <= island.commit;
        CHECK(settled.context.pc == header + island.site + (back ? 0 : 4));
        const bool x18_in_value = !back && k <= island.store;  // committed, but slot 11 not yet written
        CHECK(settled.x18 == (x18_in_value ? worked : slot11));
        CHECK(settled.context.x[value] == worked);    // neither dead register is restored from the red zone
        CHECK(settled.context.x[base] == had_base);
    }
}

// A container older than APK_CONTAINER_RESUMABLE_VERSION has restartable, not resumable, EXCLUSIVE
// windows: settle() reads such an island the old way (back to the site before the commit, forward to
// site + 4 * resume after it), as it did before X4h, so an old cached container still runs correctly.
TEST(an_older_container_reads_an_exclusive_island_the_restartable_way) {
    constexpr std::uint8_t commit = 4, forward = 5, value = 9, base = 10;
    apk_island island{};
    island.stub = 0x1000;
    island.site = 0x400;
    island.words = 12;
    island.kind = APK_ISLAND_EXCLUSIVE;
    island.value = value;
    island.base = base;
    island.commit = commit;   // a commit point, the old meaning
    island.store = commit + 2;
    island.resume = forward;  // words to skip forward after the commit, the old meaning
    constexpr std::uint64_t header = 0x1'0000'0000;
    constexpr std::uint64_t worked = 0xbeef, saved_value = 0xcafe, saved_base = 0xf00d, slot11 = 0x1818;
    for (unsigned k = 0; k < island.words; ++k) {
        apkrt::GuestContext context{};
        context.pc = header + island.stub + 4 * k;
        context.x[value] = worked;
        const auto settled = apkrt::islands::settle(island, header, context, saved_value, saved_base, slot11, false);
        // Back to the site up to and including the commit; forward to site + 4 * resume after it.
        const bool back = k <= commit;
        CHECK(settled.context.pc == header + island.site + (back ? 0 : 4 * std::uint64_t{forward}));
        const bool x18_in_value = !back && k <= island.store;
        CHECK(settled.x18 == (x18_in_value ? worked : slot11));
    }
}

// The dual path through the runtime's own entry points: a registered version-3 container's EXCLUSIVE
// island must go through leave() the restartable way (leave derives resumable from the container
// version), and reenter() must skip it (ADR-0020 X4h). Kills the mutants of leave() always passing
// resumable = true and reenter() dropping its version skip.
TEST(an_older_container_through_leave_and_reenter_stays_restartable) {
    alignas(4096) static std::uint8_t backing[0x2000]{};
    const auto header = reinterpret_cast<std::uint64_t>(backing);
    static apk_container_info info{};
    info.version = 3;  // older than APK_CONTAINER_RESUMABLE_VERSION
    info.islands = 0x100;
    info.island_count = 1;
    info.stubs = 0x200;
    info.stubs_size = 0x100;
    info.image_offset = 0x1000;
    info.image_size = 0x1000;
    auto &island = *reinterpret_cast<apk_island *>(backing + info.islands);
    island.stub = 0x200;          // header-relative, in the stub area
    island.site = 0x1040;         // header-relative, in the image
    island.words = 10;
    island.kind = APK_ISLAND_EXCLUSIVE;
    island.value = 9;
    island.base = 10;
    island.commit = 4;            // a commit point, the old (restartable) meaning
    island.store = 6;
    island.resume = 5;            // words to skip forward after the commit, the old meaning
    apkrt::islands::add(backing, info, "older");

    alignas(16) std::uint64_t stack[64];
    apkrt::GuestContext context{};
    context.sp = reinterpret_cast<std::uint64_t>(&stack[32]);
    auto *red = reinterpret_cast<std::uint64_t *>(context.sp - 128);
    red[0] = 0xcafe;
    red[1] = 0xf00d;
    context.pc = header + island.stub + 4 * (std::uint64_t{island.commit} + 1);  // one word past the commit
    context.x[island.value] = 0x1234;
    std::uint64_t x18 = 0x1818;
    CHECK(apkrt::islands::leave(context, x18));
    // Restartable: forward to site + 4 * resume, not the resumable per-word map (site + 4 * (word - commit)).
    CHECK(context.pc == header + island.site + 4 * std::uint64_t{island.resume});

    apkrt::GuestContext body{};
    body.sp = reinterpret_cast<std::uint64_t>(&stack[32]);
    body.pc = header + island.site + 4;  // a body pc; a resumable container would remap it, a v3 one must not
    CHECK(!apkrt::islands::reenter(body, 0x1818));
    CHECK(body.pc == header + island.site + 4);
}

// ADR-0020 X4h: reenter() turns a guest pc inside a relocated window body (past its load-exclusive)
// into the matching island word, puts the guest x18 into the value scratch and rebuilds the red zone
// the first frame clobbered; the window start (site + 0, the branch) and the word after the window are
// left alone. Uses the real program's window island so the fields come from the converter.
TEST(reenter_redirects_a_window_body_pc_into_the_island_and_restores_its_scratch) {
    const auto &loaded = program();
    const auto &island = loaded.island_at(window_at);  // the LL/SC window that stores x18 (writes x18)
    CHECK(island.kind == APK_ISLAND_EXCLUSIVE && island.base != APK_ISLAND_NO_REGISTER);
    alignas(16) std::uint64_t stack[64];
    const auto sp = reinterpret_cast<std::uint64_t>(&stack[32]);  // the red zone at sp - 128 is &stack[16]
    auto *red_zone = reinterpret_cast<std::uint64_t *>(sp - 128);
    constexpr std::uint64_t guest_value = 0xaaaa, guest_base = 0xbbbb, x18 = 0x1818;
    for (unsigned guest_word = 0; guest_word <= island.resume; ++guest_word) {
        apkrt::GuestContext context{};
        context.sp = sp;
        context.pc = loaded.header + island.site + 4 * guest_word;
        context.x[island.value] = guest_value;
        context.x[island.base] = guest_base;
        red_zone[0] = red_zone[1] = 0;  // as the signal frame left it
        const bool redirected = apkrt::islands::reenter(context, x18);
        if (guest_word == 0 || guest_word >= island.resume) {
            CHECK(!redirected);  // site + 0 is the branch into the island; the window end is guest code
            CHECK(context.pc == loaded.header + island.site + 4 * guest_word);
        } else {
            CHECK(redirected);
            CHECK(context.pc == loaded.header + island.stub + 4 * (std::uint64_t{island.commit} + guest_word));
            CHECK(context.x[island.value] == x18);       // the value scratch carries the guest x18
            CHECK(red_zone[0] == guest_value);           // the red zone holds the guest scratch again
            CHECK(red_zone[1] == guest_base);
        }
    }
}

// A window that only reads x18 has no second scratch register (base == APK_ISLAND_NO_REGISTER) and
// never saves anything at sp - 120, so reenter() must leave that red-zone slot untouched: rebuilding
// it would read context.x[255] and clobber a word the island does not own.
TEST(reenter_leaves_the_base_slot_alone_for_a_window_that_does_not_write_x18) {
    const auto &loaded = program();
    const auto &island = loaded.island_at(fault_window_deep_at);  // reads x18 as an address, never writes it
    CHECK(island.kind == APK_ISLAND_EXCLUSIVE && island.base == APK_ISLAND_NO_REGISTER);
    CHECK(island.resume >= 2);  // at least one body word strictly inside the window
    alignas(16) std::uint64_t stack[64];
    const auto sp = reinterpret_cast<std::uint64_t>(&stack[32]);  // the red zone at sp - 128 is &stack[16]
    auto *red_zone = reinterpret_cast<std::uint64_t *>(sp - 128);
    constexpr std::uint64_t guest_value = 0xaaaa, x18 = 0x1818, sentinel = 0x5555'5555'5555'5555;
    apkrt::GuestContext context{};
    context.sp = sp;
    context.pc = loaded.header + island.site + 4;  // the first word inside the window body
    context.x[island.value] = guest_value;
    red_zone[0] = 0;
    red_zone[1] = sentinel;  // the island of a read-only window never writes sp - 120
    CHECK(apkrt::islands::reenter(context, x18));
    CHECK(context.x[island.value] == x18);
    CHECK(red_zone[0] == guest_value);
    CHECK(red_zone[1] == sentinel);  // untouched: base is APK_ISLAND_NO_REGISTER
}

// The indirect islands (ADR-0020 X2e): BLR x18 to a helper that returns to the call site, then a
// tail BR x18. The call completes only if the island branched to the guest x18 and the BLR island
// set the return address to the word after the site; and wherever a signal stops a thread inside
// either island, the runtime reconstructs the guest state at the site, with x16 (and, for the BLR,
// x30) restored from the red zone.
TEST(the_indirect_islands_branch_to_x18_and_leave_to_a_real_guest_state) {
    const auto &loaded = program();
    Covered covered;
    int compared = 0;
    std::uint64_t inputs[2] = {};
    const auto result = step_and_compare(
        indirect_at,
        [&] {
            inputs[0] = loaded.image + helper_at;  // the blr target
            inputs[1] = loaded.image + tail_at;    // the br target
            return reinterpret_cast<std::uint64_t>(inputs);
        },
        {}, covered, compared);
    CHECK(result == 13);  // helper set x2 = 10, returned to 0x354, +3 = 13, the tail returned it
    CHECK(compared > 0);
    // Both indirect islands were reached, and every one of their words was stepped and reconstructed.
    for (const auto &island : loaded.islands) {
        const auto site = loaded.header + island.site - loaded.image;
        if (site != 0x354 && site != 0x364) continue;  // the blr and br sites
        CHECK(island.kind == APK_ISLAND_INDIRECT);
        for (unsigned word = 0; word < island.words; ++word) {
            if (!covered.contains({&island, word})) {
                apk::testing::fail(std::source_location::current(), "never stepped: ",
                                   std::format("site {:#x}, word {}", island.site, word).c_str());
            }
        }
    }
}

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, asm-generic/signal.h, errno.h.
constexpr std::uint64_t pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr std::uint64_t nr_futex = 98, nr_tgkill = 131, nr_rt_sigaction = 134, nr_gettid = 178;
constexpr std::uint64_t sigtrap = 5, sigusr1 = 10, sigsegv = 11;
constexpr std::uint64_t sa_siginfo = 0x4, sa_restorer = 0x0400'0000;
constexpr long eagain = 11;
// The Linux ucontext on arm64: struct sigcontext's pc after regs[31] and sp (signals.cpp).
constexpr std::size_t linux_registers_offset = 184, linux_pc_offset = 440;
constexpr std::size_t stack_size = 256 * 1024;

void *map_stack() {
    void *stack = ::mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return stack == MAP_FAILED ? nullptr : stack;
}

void *top_of(void *stack) {
    return static_cast<char *>(stack) + stack_size;
}

/// pthread_join's wait: until the kernel clears the thread ID word at the thread's exit.
void join(int *tid_word) {
    for (int value; (value = __atomic_load_n(tid_word, __ATOMIC_ACQUIRE)) != 0;) {
        const long result = probe_syscall(nr_futex, reinterpret_cast<std::uint64_t>(tid_word), 0 /* FUTEX_WAIT */,
                                          static_cast<std::uint64_t>(value), 0);
        CHECK(result == 0 || result == -eagain);
    }
}

void set_action(std::uint64_t signal, void (*handler)(int, void *, void *)) {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } action{reinterpret_cast<std::uint64_t>(handler), sa_siginfo | sa_restorer,
             reinterpret_cast<std::uint64_t>(&probe_sigreturn), 0};
    CHECK(probe_syscall(nr_rt_sigaction, signal, reinterpret_cast<std::uint64_t>(&action), 0, 8) == 0);
}

void set_default_action(std::uint64_t signal) {
    const std::uint64_t action[4] = {};  // SIG_DFL
    CHECK(probe_syscall(nr_rt_sigaction, signal, reinterpret_cast<std::uint64_t>(action), 0, 8) == 0);
}

/// Whether `pc` lies inside an x18 island of a loaded container.
bool in_island(std::uint64_t pc) {
    const auto *container = apkrt::islands::container_at(pc);
    return container != nullptr && apkrt::islands::island_at(*container, pc) != nullptr;
}

struct FaultView {
    std::uint64_t pc = 0;              // what the handler's frame said
    std::uint64_t address = 0;         // the fault address in its siginfo
    std::uint64_t registers[31] = {};  // the frame's x0 to x30
    std::uint64_t resume_at = 0;       // where the handler sends the thread on, or 0: past the load
    std::uint64_t result = 0;          // what the call returned
    std::uint64_t x18_after = 0;
};
FaultView fault_view;
constexpr std::uint64_t unmapped = 0x10;  // below 4 GiB: nothing is mapped there for the guest
constexpr std::uint64_t marker = 0x6d61'726b;

void on_segv(int, void *info, void *ucontext) {
    fault_view.address = static_cast<const apkrt::signals::LinuxSiginfo *>(info)->fields.address;
    std::memcpy(fault_view.registers, static_cast<const char *>(ucontext) + linux_registers_offset,
                sizeof fault_view.registers);
    auto &pc = *reinterpret_cast<std::uint64_t *>(static_cast<char *>(ucontext) + linux_pc_offset);
    fault_view.pc = pc;
    pc = fault_view.resume_at != 0 ? fault_view.resume_at : pc + 4;
}

// A handler that, on the first fault (x18 unmapped), gives x18 a mapped address and retries the
// faulting instruction (pc unchanged); a later fault, or one at address 0, means the retry ran the
// original body with x18 = 0 (Finding B) -- it is recorded and the handler escapes to end the test.
struct RetryView {
    int faults = 0;
    std::uint64_t fix_x18 = 0;  // the mapped address to put in x18 on the first fault
    std::uint64_t escape = 0;   // where to send the thread to end the test after a bad retry
    bool saw_zero = false;      // a fault at address 0: the retry ran with x18 = 0
};
RetryView retry_view;

void on_segv_retry(int, void *info, void *ucontext) {
    const auto address = static_cast<const apkrt::signals::LinuxSiginfo *>(info)->fields.address;
    auto &pc = *reinterpret_cast<std::uint64_t *>(static_cast<char *>(ucontext) + linux_pc_offset);
    auto *regs = reinterpret_cast<std::uint64_t *>(static_cast<char *>(ucontext) + linux_registers_offset);
    if (++retry_view.faults > 1 || address == 0) {
        retry_view.saw_zero = retry_view.saw_zero || address == 0;
        pc = retry_view.escape;  // break out of the retry so the test ends
        return;
    }
    regs[18] = retry_view.fix_x18;  // give x18 a mapped address and retry the faulting instruction
}

struct FaultCall {
    std::uint64_t function_at;
    std::uint64_t argument;
    std::uint64_t x18;
};

void fault_child(void *raw) {
    const auto &call = *static_cast<const FaultCall *>(raw);
    apkrt_set_guest_x18(call.x18);
    fault_view.result = reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(program().image + call.function_at)(call.argument);
    fault_view.x18_after = apkrt_get_guest_x18();
}

/// Runs `call` on a guest thread whose SIGSEGV handler is on_segv, and returns what it saw.
FaultView fault_in(FaultCall call, std::uint64_t resume_at = 0) {
    fault_view = {};
    fault_view.resume_at = resume_at;
    set_action(sigsegv, on_segv);
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, fault_child, &call) > 0);
    join(&tid_word);
    set_default_action(sigsegv);
    ::munmap(stack, stack_size);
    return fault_view;
}

// ADR-0020 X4: a fault inside an island reaches the guest's handler at the site, with the fault
// address the instruction would have faulted on; the thread goes on from there.
TEST(a_fault_inside_an_island_shows_the_handler_its_site) {
    const auto seen = fault_in({fault_at, unmapped, 0x1818});
    CHECK(seen.pc == program().image + fault_at);
    CHECK(seen.address == unmapped);
    CHECK(seen.x18_after == 0x1818);  // the load never happened
}

// The handler sees the guest's registers too, where the island had already loaded x18 into its
// value register when the instruction faulted (found missing for X4).
TEST(a_fault_after_the_island_loaded_x18_shows_the_handler_the_guest_registers) {
    const auto seen = fault_in({fault_read_at, marker, unmapped});
    CHECK(seen.pc == program().image + fault_read_at + 4);  // the ldr
    CHECK(seen.address == unmapped);
    CHECK(seen.registers[1] == marker);  // the value register has the guest's x1 again
    CHECK(seen.registers[18] == unmapped);
    CHECK(seen.result == marker && seen.x18_after == unmapped);
}

// A fault inside an LL/SC window reaches the handler at its load-exclusive, with the guest's
// registers. The handler sends the thread on to the ret behind the window's loop.
TEST(a_fault_inside_an_exclusive_window_shows_the_handler_its_load_exclusive) {
    const auto image = program().image;
    const auto seen = fault_in({fault_window_at, marker, unmapped}, image + fault_window_at + 16);
    CHECK(seen.pc == image + fault_window_at);
    CHECK(seen.address == unmapped);
    CHECK(seen.registers[0] == marker);  // the window's value register has the guest's x0 again
    CHECK(seen.registers[18] == unmapped);
    CHECK(seen.result == marker && seen.x18_after == unmapped);
}

// ADR-0020 X4h: a fault on a guest load deeper in a relocated window reaches the handler at that
// instruction, not at the window's start -- resumability delivers the signal where it happened, the
// path ART's implicit null check needs. This window relocates only because X4h allows the load.
TEST(a_fault_deep_in_a_relocated_window_shows_the_handler_that_instruction) {
    static std::uint64_t cell = 0;
    const auto image = program().image;
    const auto seen = fault_in({fault_window_deep_at, reinterpret_cast<std::uint64_t>(&cell), unmapped},
                               image + fault_window_deep_at + 16);  // resume at the ret, past the loop
    CHECK(seen.pc == image + fault_window_deep_at + 4);  // the ldr x9, [x18], the window's second word
    CHECK(seen.address == unmapped);
    CHECK(seen.registers[18] == unmapped);
}

// ADR-0020 X4h: when a handler returns to retry an instruction inside a relocated window, the runtime
// re-enters the island (where x18 lives in the value scratch), not the original body -- which iOS has
// run with x18 = 0 after clearing it at signal return. So the handler's new x18 takes effect and the
// load does not fault again at address 0. The window uses x18 as an address, the case Finding B broke.
TEST(a_fault_retry_in_a_relocated_window_re_enters_the_island_with_the_handlers_x18) {
    static std::uint64_t cell0 = 0, cell18 = 0x5a5a;
    const auto image = program().image;
    retry_view = {};
    retry_view.fix_x18 = reinterpret_cast<std::uint64_t>(&cell18);
    retry_view.escape = image + fault_window_deep_at + 16;  // the ret, if a bad retry needs breaking out
    set_action(sigsegv, on_segv_retry);
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    FaultCall call{fault_window_deep_at, reinterpret_cast<std::uint64_t>(&cell0), unmapped};
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, fault_child, &call) > 0);
    join(&tid_word);
    set_default_action(sigsegv);
    ::munmap(stack, stack_size);
    CHECK(retry_view.faults == 1);  // only the initial unmapped-x18 fault; the retry re-entered the island
    CHECK(!retry_view.saw_zero);    // the retry never ran the original body with x18 = 0 (Finding B)
}

// ADR-0020 X4f: the message names the br by its library and ELF address, and the target the guest
// computed, with its ELF address when it lies in the image.
TEST(describe_names_the_br_and_where_it_jumps) {
    apk_container_info info{};
    info.image_offset = 0x8000;
    info.image_size = 0x4000;
    info.min_vaddr = 0x1000;
    const apkrt::islands::Container container{.header = 0x1'0000'0000, .info = &info, .name = "libgame.so",
                                              .next = nullptr};
    apk_island island{};
    island.site = 0x8000 + 0x254;  // ELF address 0x1254
    island.kind = APK_ISLAND_JUMP_TABLE;
    char message[256];
    const auto inside = apkrt::islands::describe({&container, &island, 0x1'0000'8000 + 0x25c}, message, sizeof message);
    CHECK(std::string_view(message, inside) ==
          "apkrt: br x18 at libgame.so+0x1254 jumps to 0x10000825c (libgame.so+0x125c), which its jump table does "
          "not hold (ADR-0020)\n");
    const auto outside = apkrt::islands::describe({&container, &island, 0x1234}, message, sizeof message);
    CHECK(std::string_view(message, outside) ==
          "apkrt: br x18 at libgame.so+0x1254 jumps to 0x1234, which its jump table does not hold (ADR-0020)\n");
    char small[24];
    std::memset(small, 'x', sizeof small);
    const auto cut = apkrt::islands::describe({&container, &island, 0x1234}, small, 16);
    CHECK(cut == 16 && std::string_view(small, 16) == "apkrt: br x18 at" && small[16] == 'x');  // cut, never past
}

// The longest message, with names of 63 characters and addresses of 16 hexadecimal digits, fits the
// buffer the fault handler gives describe() (found for X4).
TEST(the_longest_message_fits_the_handlers_buffer) {
    apk_container_info info{};
    info.image_offset = 0x8000;
    info.image_size = 0x4000;
    info.min_vaddr = 0xf000'0000'0000'0000;
    apkrt::islands::Container container{.header = 0xf000'0000'0000'0000, .info = &info, .name = {}, .next = nullptr};
    std::memset(container.name, 'n', sizeof container.name - 1);
    apk_island island{};
    island.site = 0x8000 + 0x254;
    island.kind = APK_ISLAND_JUMP_TABLE;
    char message[apkrt::islands::message_capacity];
    const auto length =
        apkrt::islands::describe({&container, &island, container.header + 0x8000 + 0x25c}, message, sizeof message);
    CHECK(std::string_view(message, length).ends_with("+0xf00000000000025c), which its jump table does not hold (ADR-0020)\n"));
}

constexpr std::uint64_t br_at = 0x254;
constexpr std::uint64_t no_case_at = 0x25c;  // within the span of the jump table's targets, but no case

void on_sigtrap(int, void *, void *) {
    static const char message[] = "the guest's SIGTRAP handler ran\n";
    static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
    ::_exit(42);
}

void trap_child(void *) {
    apkrt_set_guest_x18(program().image + no_case_at);
    reinterpret_cast<void (*)()>(program().image + br_at)();  // the site of the br: into its island
}

/// In the child process: a guest thread whose br x18 goes to a word without a landing pad.
int run_jump_table_trap() {
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    set_action(sigtrap, on_sigtrap);
    void *stack = map_stack();
    int tid_word = 0;
    if (probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, trap_child, nullptr) <= 0) return 2;
    join(&tid_word);
    return 3;  // the trap should have ended the process
}

// ADR-0020 X4f: a br x18 to a target without a landing pad ends the process by SIGTRAP, with a
// message that names the br and the target. The guest's SIGTRAP handler does not run: it would
// return to the same brk forever.
TEST(a_jump_to_a_word_without_a_landing_pad_ends_the_process_with_a_message) {
    int pipe_ends[2];
    CHECK(::pipe(pipe_ends) == 0);
    // A scratch directory of its own: rewriting the program's container here would pull the pages
    // from under this process, which has it loaded.
    const auto child_scratch = (scratch / "trap").string();
    char mode[] = "--jump-table-trap";
    char *arguments[] = {const_cast<char *>(self_path), const_cast<char *>(child_scratch.c_str()), mode, nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    pid_t child = 0;
    CHECK(::posix_spawn(&child, self_path, &actions, nullptr, arguments, nullptr) == 0);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_ends[1]);
    std::string output;
    char chunk[512];
    for (ssize_t got; (got = ::read(pipe_ends[0], chunk, sizeof chunk)) > 0;) {
        output.append(chunk, static_cast<std::size_t>(got));
    }
    ::close(pipe_ends[0]);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTRAP);
    CHECK(output.find("apkrt: br x18 at islands-program+0x254 jumps to 0x") != std::string::npos);
    CHECK(output.find(" (islands-program+0x25c), which its jump table does not hold (ADR-0020)\n") != std::string::npos);
    CHECK(output.find("the guest's SIGTRAP handler ran") == std::string::npos);
}

// ADR-0020 X4: the stepper cannot run the jump table's trap (words 13 to 15) without ending the
// call, so leave() gets a thread there by hand: it goes back to the br with both registers restored,
// whatever they hold now, and keeps its x18 (found missing for X4).
TEST(a_thread_in_the_jump_table_trap_leaves_to_the_br) {
    const auto &loaded = program();
    const auto &island = loaded.island_at(br_at);
    const std::uint64_t red_zone[2] = {0x1111, 0x2222};  // what words 0 and 1 saved
    for (unsigned word = jump_words_to_br; word < jump_pads_at; ++word) {
        apkrt::GuestContext context{};
        for (int index = 0; index < 31; ++index) context.x[index] = 0x100 + static_cast<std::uint64_t>(index);
        context.sp = reinterpret_cast<std::uint64_t>(red_zone) + 128;
        context.pc = loaded.header + island.stub + 4 * word;
        auto expected = context;
        expected.x[island.value] = red_zone[0];
        expected.x[island.base] = red_zone[1];
        expected.pc = loaded.image + br_at;
        std::uint64_t x18 = loaded.image + no_case_at;
        CHECK(apkrt::islands::leave(context, x18));
        CHECK(std::memcmp(context.x, expected.x, sizeof context.x) == 0);
        CHECK(context.sp == expected.sp && context.pc == expected.pc);
        CHECK(x18 == loaded.image + no_case_at);
    }
}

struct Seen {
    static constexpr int capacity = 4096;
    std::atomic<int> count{0};
    std::uint64_t pcs[capacity] = {};
};
Seen seen;

void on_signal(int, void *, void *ucontext) {
    const int index = seen.count.fetch_add(1);
    if (index < Seen::capacity) {
        seen.pcs[index] = *reinterpret_cast<const std::uint64_t *>(static_cast<const char *>(ucontext) + linux_pc_offset);
    }
}

struct LoopView {
    std::atomic<long> tid{0};
    std::atomic<bool> stop{false};
    std::uint64_t counter = 0;
    std::uint64_t rounds = 0;
    std::uint64_t x18_after = 0;
};
constexpr std::uint64_t x18_start = 1'000'000;
constexpr std::uint64_t rounds_per_call = 1000;

void loop_child(void *argument) {
    auto &view = *static_cast<LoopView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    apkrt_set_guest_x18(x18_start);
    const auto loop = reinterpret_cast<void (*)(std::uint64_t, std::uint64_t *)>(program().image + loop_at);
    while (!view.stop.load()) {
        loop(rounds_per_call, &view.counter);
        view.rounds += rounds_per_call;
    }
    view.x18_after = apkrt_get_guest_x18();
}

// ADR-0020 X4: a signal never shows the guest a pc inside an x18 island, and the loop still computes
// what it computes without signals.
TEST(signals_from_another_thread_never_stop_the_guest_inside_an_island) {
    set_action(sigusr1, on_signal);
    static LoopView view;
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, loop_child, &view) > 0);
    while (view.tid.load() == 0) std::this_thread::yield();
    std::uint64_t random = 0x9e37'79b9'7f4a'7c15;
    for (int sent = 0; sent < 1000; ++sent) {
        CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), static_cast<std::uint64_t>(view.tid.load()),
                            sigusr1) == 0);
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(random % 20'000);
        while (std::chrono::steady_clock::now() < until) {
        }
    }
    view.stop = true;
    join(&tid_word);
    set_default_action(sigusr1);
    ::munmap(stack, stack_size);

    const auto rounds = view.rounds;
    CHECK(rounds > 0);
    CHECK(view.x18_after == x18_start + rounds);
    CHECK(view.counter == rounds * x18_start + rounds * (rounds + 1) / 2);
    const int handled = std::min(seen.count.load(), Seen::capacity);
    CHECK(handled > 0);
    int at_sites = 0;  // signals that found the thread in an island, or right where one ends
    for (int index = 0; index < handled; ++index) {
        const auto pc = seen.pcs[index];
        CHECK(!in_island(pc));
        bool at_site = false;
        for (const auto &island : program().islands) {
            const auto site = program().header + island.site;
            at_site = at_site || pc == site || pc == site + 4 * std::uint64_t{island.resume};
        }
        at_sites += at_site;
    }
    CHECK(at_sites > 0);  // the signals did stop the thread inside islands
}

// ADR-0020 X4h, finding A: a relocated window with a back-edge to its own load-exclusive accumulates
// x18 across passes while slot 11 stays stale until the loop's one store-back. A signal at the
// load-exclusive on a pass after the first must settle x18 from the value register (and leave() write
// that into slot 11), so the resume through the window start reloads the accumulated x18, not the stale
// slot 11. The thread does a fixed number of passes while another bombards it with signals; x18 must
// have moved by exactly that many.
struct BackEdgeView {
    std::atomic<long> tid{0};
    std::atomic<bool> stop{false};
    std::uint64_t cell = 0;
    std::uint64_t rounds = 0;
    std::uint64_t x18_after = 0;
};
constexpr std::uint64_t back_edge_x18_start = 500'000;
constexpr std::uint64_t back_edge_rounds_per_call = 1000;

void back_edge_child(void *argument) {
    auto &view = *static_cast<BackEdgeView *>(argument);
    view.tid = probe_syscall(nr_gettid, 0);
    apkrt_set_guest_x18(back_edge_x18_start);
    const auto backedge = reinterpret_cast<void (*)(std::uint64_t *, std::uint64_t)>(program().image + backedge_at);
    while (!view.stop.load()) {
        backedge(&view.cell, back_edge_rounds_per_call);
        view.rounds += back_edge_rounds_per_call;
    }
    view.x18_after = apkrt_get_guest_x18();
}

TEST(a_signal_at_a_back_edge_load_exclusive_keeps_the_accumulated_x18) {
    set_action(sigusr1, on_signal);
    static BackEdgeView view;
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word, back_edge_child, &view) > 0);
    while (view.tid.load() == 0) std::this_thread::yield();
    std::uint64_t random = 0x2545'f491'4f6c'dd1d;
    for (int sent = 0; sent < 2000; ++sent) {
        CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()),
                            static_cast<std::uint64_t>(view.tid.load()), sigusr1) == 0);
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(random % 10'000);
        while (std::chrono::steady_clock::now() < until) {
        }
    }
    view.stop = true;
    join(&tid_word);
    set_default_action(sigusr1);
    ::munmap(stack, stack_size);
    CHECK(view.rounds > 0);
    CHECK(view.x18_after - back_edge_x18_start == view.rounds);  // no pass's increment was dropped (finding A)
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: islands_test <scratch-directory> [--jump-table-trap]\n");
        return 2;
    }
    self_path = argv[0];
    // dyld records the canonical path of a loaded image, and the loader looks images up by path.
    std::filesystem::create_directories(argv[1]);
    scratch = std::filesystem::canonical(argv[1]);
    if (argc == 3 && std::strcmp(argv[2], "--jump-table-trap") == 0) return run_jump_table_trap();
    // The test's own thread is the program's first thread: the process lives as long as it does.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    return apk::testing::run_all();
}
