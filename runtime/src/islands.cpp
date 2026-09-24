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

}  // namespace apkrt::islands
