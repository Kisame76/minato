#include "islands.h"

#include <algorithm>
#include <atomic>

namespace apkrt::islands {
namespace {

static_assert(sizeof(apk_island) == 16);

/// The registered containers, the newest first. Entries are only ever added.
std::atomic<const Container *> first{nullptr};

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

}  // namespace apkrt::islands
