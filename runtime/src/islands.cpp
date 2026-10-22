#include "islands.h"

#include <algorithm>
#include <atomic>

namespace apkrt::islands {
namespace {

static_assert(sizeof(apk_island) == 16);

constexpr std::uint32_t jump_table_brk = 0xd420'0300;  // BRK #0x18: a target without a landing pad

/// The registered containers, the newest first. Entries are only ever added.
std::atomic<const Container *> first{nullptr};

/// Writes text and hexadecimal numbers into a fixed buffer, cut at its end, without allocating.
class Writer {
public:
    Writer(char *buffer, std::size_t size) : buffer_(buffer), size_(size) {}

    Writer &text(std::string_view text) {
        for (const char character : text) put(character);
        return *this;
    }

    Writer &hex(std::uint64_t value) {
        char digits[16];
        int count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 0xf];
            value >>= 4;
        } while (value != 0);
        text("0x");
        while (count > 0) put(digits[--count]);
        return *this;
    }

    std::size_t length() const { return length_; }

private:
    void put(char character) {
        if (length_ < size_) buffer_[length_++] = character;
    }

    char *buffer_;
    std::size_t size_;
    std::size_t length_ = 0;
};

}  // namespace

void add(const void *header, const apk_container_info &info, std::string_view name) {
    auto *container = new Container{.header = reinterpret_cast<std::uint64_t>(header), .info = &info, .name = {},
                                    .next = nullptr};
    name.copy(container->name, sizeof container->name - 1);
    // Published with a release store; container_at() walks the list with acquire loads.
    const auto *head = first.load(std::memory_order_relaxed);
    do {
        container->next = head;
    } while (!first.compare_exchange_weak(head, container, std::memory_order_release, std::memory_order_relaxed));
}

const Container *container_at(std::uint64_t pc) {
    for (const auto *container = first.load(std::memory_order_acquire); container != nullptr;
         container = container->next) {
        if (pc - (container->header + container->info->stubs) < container->info->stubs_size) return container;
    }
    return nullptr;
}

const apk_island *island_at(const Container &container, std::uint64_t pc) {
    const auto offset = pc - container.header;
    const auto *table = reinterpret_cast<const apk_island *>(container.header + container.info->islands);
    const auto *end = table + container.info->island_count;
    // The last island that starts at or before `offset`, if `offset` lies within it.
    const auto *after = std::upper_bound(table, end, offset,
                                         [](std::uint64_t at, const apk_island &island) { return at < island.stub; });
    if (after == table) return nullptr;
    const auto &island = *(after - 1);
    return offset - island.stub < 4 * std::uint64_t{island.words} ? &island : nullptr;
}

Settled settle(const apk_island &island, std::uint64_t header, const GuestContext &context,
               std::uint64_t saved_value, std::uint64_t saved_base, std::uint64_t x18, bool resumable) {
    const std::uint64_t word = (context.pc - (header + island.stub)) / 4;
    Settled settled{context, x18};
    // x18 is in the value register until the island stores it back to slot 11; read it from the
    // current context before the red-zone restore below gives that register its guest value again.
    // Only a resumable EXCLUSIVE window uses the per-word map; an older container's EXCLUSIVE window is
    // restartable and reads like any other island (the `else` below), as it did before X4h.
    if (island.kind == APK_ISLAND_EXCLUSIVE && resumable) {
        // A relocated window is resumable: the thread returns to the exact guest instruction it was
        // stopped at (ADR-0020 X4h). `commit` is the preamble length, `resume` the window's words.
        const std::uint64_t preamble = island.commit, body = island.resume;
        // A window that writes x18 loads it into the value register in its preamble (loads_x18), so from
        // the first body word (word == preamble, the load-exclusive) the value register holds the guest
        // x18 -- equal to slot 11 on the first pass, and the accumulated x18 on a later pass reached by a
        // back-edge to the load-exclusive, where slot 11 is stale until the store-back tail runs. So x18
        // comes from the value register across the whole [preamble, store] range (ADR-0020 X4h).
        if (word >= preamble && island.store != 0 && word <= island.store) settled.x18 = context.x[island.value];
        const std::uint64_t guest = word < preamble ? 0 : std::min<std::uint64_t>(word - preamble, body);
        settled.context.pc = header + island.site + 4 * guest;
    } else {
        // Before its commit the island runs again from the site; after it the thread goes on.
        const bool back = island.commit == APK_ISLAND_NO_COMMIT || word <= island.commit;
        if (!back && island.store != 0 && word <= island.store) settled.x18 = context.x[island.value];
        settled.context.pc = header + island.site + (back ? 0 : 4 * std::uint64_t{island.resume});
    }
    if (word >= 1) settled.context.x[island.value] = saved_value;
    if (island.base != APK_ISLAND_NO_REGISTER && word >= 2) settled.context.x[island.base] = saved_base;
    return settled;
}

bool leave(GuestContext &context, std::uint64_t &x18) {
    const auto *container = container_at(context.pc);
    if (container == nullptr) return false;
    const auto *island = island_at(*container, context.pc);
    if (island == nullptr) return false;
    const std::uint64_t word = (context.pc - (container->header + island->stub)) / 4;
    // The island's own stores to the red zone succeeded, so reading it back cannot fault.
    const auto *red_zone = reinterpret_cast<const std::uint64_t *>(context.sp - 128);
    const auto saved_value = word >= 1 ? red_zone[0] : 0;
    const auto saved_base = word >= 2 && island->base != APK_ISLAND_NO_REGISTER ? red_zone[1] : 0;
    const bool resumable = container->info->version >= APK_CONTAINER_RESUMABLE_VERSION;
    const auto settled = settle(*island, container->header, context, saved_value, saved_base, x18, resumable);
    context = settled.context;
    x18 = settled.x18;
    return true;
}

bool reenter(GuestContext &context, std::uint64_t x18) {
    if (context.pc % 4 != 0) return false;  // a window body pc is word-aligned; a misaligned one faults as on Linux
    for (const auto *container = first.load(std::memory_order_acquire); container != nullptr;
         container = container->next) {
        if (container->info->version < APK_CONTAINER_RESUMABLE_VERSION) continue;  // no resumable windows here
        const auto image = container->header + container->info->image_offset;
        if (context.pc - image >= container->info->image_size) continue;  // not this container's guest image
        const auto offset = context.pc - container->header;  // Mach-O-header-relative, like island.site
        const auto *table = reinterpret_cast<const apk_island *>(container->header + container->info->islands);
        for (std::size_t index = 0; index < container->info->island_count; ++index) {
            const auto &island = table[index];
            if (island.kind != APK_ISLAND_EXCLUSIVE) continue;
            const std::uint64_t body_end = std::uint64_t{island.site} + 4 * std::uint64_t{island.resume};
            if (offset <= island.site || offset >= body_end) continue;  // only past the load-exclusive, in the body
            const std::uint64_t word = (offset - island.site) / 4;
            // The island's preamble, which re-entering past it skips, saved the two scratch registers in
            // the red zone and loaded the guest x18 into the value register. Rebuild both: the first
            // signal frame clobbered the red zone, so restore it from the guest scratch the context still
            // carries, then give the value register the guest x18 the body runs on (ADR-0020 X4h).
            auto *red_zone = reinterpret_cast<std::uint64_t *>(context.sp - 128);
            red_zone[0] = context.x[island.value];
            if (island.base != APK_ISLAND_NO_REGISTER) red_zone[1] = context.x[island.base];
            context.x[island.value] = x18;
            context.pc = container->header + island.stub + 4 * (std::uint64_t{island.commit} + word);
            return true;
        }
    }
    return false;
}

std::optional<JumpTableTrap> jump_table_trap(std::uint64_t pc, std::uint64_t x18) {
    const auto *container = container_at(pc);
    if (container == nullptr) return std::nullopt;
    const auto *island = island_at(*container, pc);
    if (island == nullptr || island->kind != APK_ISLAND_JUMP_TABLE) return std::nullopt;
    if (*reinterpret_cast<const std::uint32_t *>(pc) != jump_table_brk) return std::nullopt;
    return JumpTableTrap{container, island, x18};
}

std::size_t describe(const JumpTableTrap &trap, char *buffer, std::size_t size) {
    const auto &info = *trap.container->info;
    const std::string_view name(trap.container->name);
    Writer write(buffer, size);
    write.text("apkrt: br x18 at ").text(name).text("+").hex(trap.island->site - info.image_offset + info.min_vaddr);
    write.text(" jumps to ").hex(trap.target);
    const auto image = trap.container->header + info.image_offset;
    if (trap.target - image < info.image_size) {
        write.text(" (").text(name).text("+").hex(trap.target - image + info.min_vaddr).text(")");
    }
    write.text(", which its jump table does not hold (ADR-0020)\n");
    return write.length();
}

}  // namespace apkrt::islands
