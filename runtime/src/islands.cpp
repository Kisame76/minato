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
               std::uint64_t saved_value, std::uint64_t saved_base, std::uint64_t x18) {
    const std::uint64_t word = (context.pc - (header + island.stub)) / 4;
    const bool back = island.commit == APK_ISLAND_NO_COMMIT || word <= island.commit;
    Settled settled{context, x18};
    // Forward, before the island's own store: x18 is still in the value register.
    if (!back && island.store != 0 && word <= island.store) settled.x18 = context.x[island.value];
    if (word >= 1) settled.context.x[island.value] = saved_value;
    if (island.base != APK_ISLAND_NO_REGISTER && word >= 2) settled.context.x[island.base] = saved_base;
    settled.context.pc = header + island.site + (back ? 0 : 4 * std::uint64_t{island.resume});
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
    const auto settled = settle(*island, container->header, context, saved_value, saved_base, x18);
    context = settled.context;
    x18 = settled.x18;
    return true;
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
