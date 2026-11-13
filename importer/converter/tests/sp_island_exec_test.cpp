// Runs the island of a word that sets sp from x18 on the Mac (arm64), with timer signals arriving while it runs: the
// handler fills 16 KiB below its own frame, so a scratch register left below the stack pointer is lost. Every register
// x0 to x28 except x18 must come back unchanged and sp must be the value the word computes, with sp rising by more
// than the red zone, by 16 bytes, not at all, and falling. The runtime's settling of a stopped thread is not part of
// this test; it needs the saved registers at sp - 128 at every moment, which is what the signals check.
#include "apkconv/rewrite.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <sys/time.h>

extern "C" {
extern const char sp_pre_begin[], sp_pre_end[], sp_post_begin[], sp_post_end[];
}

namespace {

struct Context {
    std::uint64_t in[29];
    std::uint64_t init_sp;
    std::uint64_t saved_sp;
    std::uint64_t out[29];
    std::uint64_t out_sp;
    std::uint64_t in_flags;
    std::uint64_t out_flags;
};

volatile std::uint64_t signals = 0, settled_back = 0, settled_on = 0;
volatile std::int64_t g_trap = -1;
std::uint32_t g_original = 0;
std::uint64_t g_stub = 0, g_site = 0, g_words = 0, g_commit = 0, g_value = 0, g_base = 0;

// The handler does what the runtime's settling does (runtime/src/islands.cpp): a thread stopped inside the island
// goes back to the site up to and including the commit word, on to the next guest word after it, and the scratch
// registers come back from sp - 128 of the sp it has then. Then the handler fills 16 KiB below its own frame, as a
// guest handler may.
void on_alarm(int, siginfo_t *, void *raw) {
    auto *ucontext = static_cast<ucontext_t *>(raw);
    auto &state = ucontext->uc_mcontext->__ss;
    const std::uint64_t pc = __darwin_arm_thread_state64_get_pc(state);
    if (g_trap >= 0 && pc == g_stub + 4 * static_cast<std::uint64_t>(g_trap)) {  // the planted brk: put the word back
        pthread_jit_write_protect_np(0);
        std::memcpy(reinterpret_cast<void *>(pc), &g_original, 4);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(reinterpret_cast<void *>(pc), 4);
        g_trap = -1;
    }
    if (pc >= g_stub && pc < g_stub + 4 * g_words) {
        const std::uint64_t word = (pc - g_stub) / 4;
        const bool back = word <= g_commit;
        const auto *red = reinterpret_cast<const std::uint64_t *>(__darwin_arm_thread_state64_get_sp(state) - 128);
        if (word >= 1) state.__x[g_value] = red[0];
        if (word >= 2) state.__x[g_base] = red[1];
        __darwin_arm_thread_state64_set_pc_fptr(state, reinterpret_cast<void *>(back ? g_site : g_site + 4));
        (back ? settled_back : settled_on) = (back ? settled_back : settled_on) + 1;
    }
    volatile unsigned char scratch[16384];
    for (std::size_t i = 0; i < sizeof scratch; ++i) scratch[i] = 0xee;
    signals = signals + 1;
}

// The handler of a runtime that runs its handlers on a sigaltstack and does not settle (a signal it ignores, say): the
// island must be right with the thread's stack untouched, whatever lies below sp.
void on_alarm_alt(int, siginfo_t *, void *) {
    volatile unsigned char scratch[16384];
    for (std::size_t i = 0; i < sizeof scratch; ++i) scratch[i] = 0xee;
    signals = signals + 1;
}

std::uint32_t movz(unsigned rd, std::uint64_t imm, unsigned hw) {
    return 0xd280'0000u | (hw << 21) | (static_cast<std::uint32_t>(imm & 0xffff) << 5) | rd;
}
std::uint32_t movk(unsigned rd, std::uint64_t imm, unsigned hw) {
    return 0xf280'0000u | (hw << 21) | (static_cast<std::uint32_t>(imm & 0xffff) << 5) | rd;
}

struct Case {
    const char *name;
    std::uint32_t word;
    std::int64_t x18_from_init;  // the guest x18, relative to the initial sp
    std::function<std::uint64_t(std::uint64_t x18)> expected;
};

int failures = 0;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);                    \
            ++failures;                                                                        \
        }                                                                                      \
    } while (false)

}  // namespace

int main() {
    const std::size_t region_size = 4u << 20;
    auto *region = static_cast<std::uint8_t *>(
        mmap(nullptr, region_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto *jit = static_cast<std::uint8_t *>(
        mmap(nullptr, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
    if (region == MAP_FAILED || jit == MAP_FAILED) {
        std::printf("SKIP no memory\n");
        return 0;
    }
    const std::uint64_t init_sp = reinterpret_cast<std::uint64_t>(region) + region_size / 2;

    const std::vector<Case> cases = {
        {"add #0x30 rising 4 KiB", 0x9100'c25f, 0x1000 - 0x30, [](auto x) { return x + 0x30; }},
        {"add #0x30 rising 16", 0x9100'c25f, 16 - 0x30, [](auto x) { return x + 0x30; }},
        {"add #0x30 equal", 0x9100'c25f, -0x30, [](auto x) { return x + 0x30; }},
        {"add #0x30 falling 16", 0x9100'c25f, -16 - 0x30, [](auto x) { return x + 0x30; }},
        {"add #5 lsl 12 falling", 0x9140'165f, -0x8000, [](auto x) { return x + 0x5000; }},
        {"sub #0x40 falling", 0xd101'025f, 0x40 - 0x2000, [](auto x) { return x - 0x40; }},
        {"sub #3 lsl 12 equal", 0xd140'0e5f, 0x3000, [](auto x) { return x - 0x3000; }},
        {"mov rising 64 KiB", 0x9100'025f, 0x10000, [](auto x) { return x; }},
        {"mov falling 128 KiB", 0x9100'025f, -0x20000, [](auto x) { return x; }},
        {"mov rising 144", 0x9100'025f, 144, [](auto x) { return x; }},
        {"and ~15 falling", 0x927c'ee5f, -0x1000, [](auto x) { return x & ~std::uint64_t{15}; }},
    };

    const std::size_t alt_size = 256 * 1024;
    stack_t alt{};
    alt.ss_sp = mmap(nullptr, alt_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    alt.ss_size = alt_size;
    sigaltstack(&alt, nullptr);
    struct sigaction action {};
    action.sa_sigaction = on_alarm;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGALRM, &action, nullptr);
    sigaction(SIGTRAP, &action, nullptr);

    for (const auto &c : cases) {
        // Mode 0: no signals and a stop before each word; 1: timer signals on the thread's stack, settled like the
        // runtime does; 2: timer signals on a sigaltstack, not settled.
        for (const int mode : {0, 1, 2}) {
            const bool with_signals = mode != 0;
            struct sigaction timer_action {};
            timer_action.sa_sigaction = mode == 2 ? on_alarm_alt : on_alarm;
            timer_action.sa_flags = SA_SIGINFO | (mode == 2 ? SA_ONSTACK : 0);
            sigemptyset(&timer_action.sa_mask);
            sigaction(SIGALRM, &timer_action, nullptr);
            apk::rewrite::Site site{0, apk::rewrite::SiteKind::x18, 18, c.word};
            const auto layout = apk::rewrite::island_layout(site);
            CHECK(layout.kind == APK_ISLAND_SINGLE && layout.words == 13 && layout.commit == 9 && layout.resume == 1);
            const auto pre_size = static_cast<std::size_t>(sp_pre_end - sp_pre_begin);
            const auto post_size = static_cast<std::size_t>(sp_post_end - sp_post_begin);
            pthread_jit_write_protect_np(0);
            std::memcpy(jit, sp_pre_begin, pre_size);
            const auto site_address = reinterpret_cast<std::uint64_t>(jit) + pre_size;
            const auto stub_address = reinterpret_cast<std::uint64_t>(jit) + 4096;
            std::vector<std::byte> stub(apk::rewrite::stub_size(site));
            CHECK(stub.size() == 13 * 4);
            const auto patch = apk::rewrite::emit(site, site_address, stub_address, 0x1000, stub);
            std::uint32_t words[13];
            std::memcpy(words, stub.data(), sizeof words);
            g_stub = stub_address, g_site = site_address, g_words = 13, g_commit = layout.commit;
            g_value = layout.value, g_base = layout.base;
            Context context{};
            const std::uint64_t x18 = init_sp + static_cast<std::uint64_t>(c.x18_from_init);
            // The island loads the guest x18 with mrs, bic, ldr (words 2 to 4): here with three moves of a constant.
            words[2] = movz(layout.value, x18, 0);
            words[3] = movk(layout.value, x18 >> 16, 1);
            words[4] = movk(layout.value, x18 >> 32, 2);
            std::memcpy(jit + 4096, words, sizeof words);
            std::memcpy(jit + pre_size, &patch, 4);
            std::memcpy(jit + pre_size + 4, sp_post_begin, post_size);
            const auto pre_lit = reinterpret_cast<std::uint64_t>(&context);
            std::memcpy(jit + 8, &pre_lit, 8);
            std::memcpy(jit + pre_size + 4 + 8, &pre_lit, 8);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(jit, 8192);

            for (unsigned i = 0; i < 29; ++i) {
                context.in[i] = 0x1111'0000'0000'0000ull * (1 + i % 7) + 0x0101'0101ull * i + i;
            }
            context.init_sp = init_sp;
            context.in_flags = (c.word & 0x400) != 0 ? 0xa000'0000 : 0x5000'0000;  // N,C or Z,V
            using Entry = void (*)();
            auto entry = reinterpret_cast<Entry>(jit);
            const std::uint64_t expected_sp = c.expected(x18);
            std::uint64_t bad = 0;
            const unsigned iterations = with_signals ? 60000 : 1;
            if (with_signals) {
                itimerval timer{{0, 40}, {0, 40}};
                setitimer(ITIMER_REAL, &timer, nullptr);
            }
            for (unsigned n = 0; n < iterations; ++n) {
                std::memset(context.out, 0, sizeof context.out);
                context.out_sp = 0;
                entry();
                bool ok = context.out_sp == expected_sp && context.out_flags == context.in_flags;
                for (unsigned i = 0; i < 29; ++i) ok = ok && (i == 18 || context.out[i] == context.in[i]);
                if (!ok) ++bad;
            }
            if (with_signals) {
                itimerval off{};
                setitimer(ITIMER_REAL, &off, nullptr);
            }
            if (!with_signals) {  // a thread stopped before each word of the island, in turn (what a signal there does)
                for (unsigned k = 0; k < 13; ++k) {
                    const std::uint32_t brk = 0xd420'0000u;
                    std::memcpy(&g_original, jit + 4096 + 4 * k, 4);
                    pthread_jit_write_protect_np(0);
                    std::memcpy(jit + 4096 + 4 * k, &brk, 4);
                    pthread_jit_write_protect_np(1);
                    sys_icache_invalidate(jit + 4096, 64);
                    g_trap = k;
                    std::memset(context.out, 0, sizeof context.out);
                    context.out_sp = 0;
                    entry();
                    bool ok = g_trap < 0 && context.out_sp == expected_sp && context.out_flags == context.in_flags;
                    for (unsigned i = 0; i < 29; ++i) ok = ok && (i == 18 || context.out[i] == context.in[i]);
                    if (!ok) {
                        std::printf("FAIL %s stopped before word %u\n", c.name, k);
                        ++failures;
                    }
                    g_trap = -1;
                }
            }
            std::printf("%s %-26s %s bad=%llu\n", bad == 0 ? "ok  " : "FAIL", c.name,
                        mode == 2 ? "altstack" : mode == 1 ? "settled " : "plain   ",
                        static_cast<unsigned long long>(bad));
            if (bad != 0) ++failures;
        }
    }
    std::printf("signals delivered: %llu, settled back %llu, on %llu\n", static_cast<unsigned long long>(signals),
                static_cast<unsigned long long>(settled_back), static_cast<unsigned long long>(settled_on));
    return failures == 0 ? 0 : 1;
}
