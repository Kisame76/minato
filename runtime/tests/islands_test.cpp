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
};
constexpr std::int32_t jump_table[] = {0x258 - 0x400, 0x260 - 0x400, 0x268 - 0x400, 0x270 - 0x400};
constexpr std::uint64_t literal = 0x1122'3344'5566'7788;  // what the LDR (literal) of forms() loads
constexpr std::uint64_t run_at = 0x200, run_end = 0x280, window_at = 0x228, loop_at = 0x280, fault_at = 0x2a4,
                        forms_at = 0x2ac, forms_end = 0x320, cmpxchg_at = 0x2ec, fault_read_at = 0x320,
                        fault_window_at = 0x32c, table_at = 0x400, literal_at = 0x410, program_end = 0x418;

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
    builder.sections({
        {".text", elf::sht_progbits, elf::shf_alloc | elf::shf_execinstr, run_at, run_at, sizeof code},
        {".rodata", elf::sht_progbits, elf::shf_alloc, table_at, table_at, program_end - table_at},
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

/// The free run of an LL/SC window (stepper.h): from its load-exclusive to the word after its commit,
/// from visit `after_visits` of the load-exclusive on.
apkrt::testing::FreeRun free_run(const apk_island &window, unsigned after_visits) {
    CHECK(window.kind == APK_ISLAND_EXCLUSIVE);
    const auto stub = program().header + window.stub;
    unsigned word = 0;
    const auto is_load_exclusive = [](std::uint32_t instruction) { return (instruction & 0x3f40'0000) == 0x0840'0000; };
    while (word < window.commit && !is_load_exclusive(*reinterpret_cast<const std::uint32_t *>(stub + 4 * word))) ++word;
    CHECK(word < window.commit);
    return {stub + 4 * word, stub + 4 * (window.commit + 1u), after_visits};
}

/// What a window that goes back has already written, which the trace's state at its site does not
/// show: x8, which the load-exclusives of the program load, or the flags of the cmpxchg's cmp.
Ignored written_before_going_back(const apk_island &window) {
    if (program().header + window.site == program().image + cmpxchg_at) return {.flags = true};
    return {.registers = 1u << 8};
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
            // The trace has a gap: run the call again. Under load gaps come in bursts (stepper.h), so each try
            // waits a little longer first, up to 190 ms over 20 tries.
            if (attempt == 20) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt));
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
        const auto ignored = !forward && island->kind == APK_ISLAND_EXCLUSIVE ? written_before_going_back(*island)
                                                                              : Ignored{};
        const auto location = std::source_location::current();
        if (!left[at].first) {
            apk::testing::fail(location, "leave() did not see an island at ", std::format("{:#x}", pc).c_str());
        } else if (real == steps.size()) {
            apk::testing::fail(location, "the trace has no state to compare with at ", std::format("{:#x}", pc).c_str());
        } else if (const auto different = difference(left[at].second, steps[real], ignored); !different.empty()) {
            const auto detail = std::format("site {:#x}, word {} ({}): {}", island->site, word,
                                            forward ? "forward" : "back", different);
            apk::testing::fail(location, "left an island to a state the guest never had: ", detail.c_str());
        }
        ++compared;
    }
    return result;
}

// ADR-0020 X4: wherever a signal stops a thread inside an island, the state the runtime computes
// equals the state the guest really passed through in the direction the trace shows: the state after
// the island once the commit word has run (for a window: its store-exclusive, which may have failed),
// else the state at the site. All registers, sp, pc, flags and x18 count; only for a window that goes
// back, what the window has written does not (ADR-0020, X4 item 7).
TEST(every_word_of_every_island_leaves_to_a_real_guest_state) {
    const auto &loaded = program();
    Covered covered;
    int compared = 0;
    std::uint64_t counter = 0, counter_before = 0;

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

    CHECK(compared > 0);
    // Every word of every island of run() and forms() that can run was stepped.
    for (const auto &island : loaded.islands) {
        const auto site = loaded.header + island.site - loaded.image;
        if (!(site >= run_at && site < run_end) && !(site >= forms_at && site < forms_end)) continue;
        for (unsigned word = 0; word < island.words; ++word) {
            if (runs(island, word) && !covered.contains({&island, word})) {
                const auto detail = std::format("site {:#x}, word {}", island.site, word);
                apk::testing::fail(std::source_location::current(), "never stepped: ", detail.c_str());
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
// value register when the instruction faulted (found missing in the review of X4).
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
// buffer the fault handler gives describe() (found in the review of X4).
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
// whatever they hold now, and keeps its x18 (found missing in the review of X4).
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
