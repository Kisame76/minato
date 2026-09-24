// The x18 islands of the loaded containers (ADR-0020 X4). A thread that a signal stops inside an
// island is sent back to the guest before any handler sees it; for that, the signal handler looks
// up the pc here.
#pragma once

#include "APKRuntime/container_format.h"

#include <cstdint>
#include <string_view>

namespace apkrt::islands {

/// A loaded container. Its metadata and island table lie in its __TEXT, which stays mapped for the
/// life of the process: containers are never unloaded (ADR-0003).
struct Container {
    std::uint64_t header;            // address of the Mach-O header
    const apk_container_info *info;  // its __apk_info
    char name[64];                   // the file name without ".dylib", for messages
    const Container *next;
};

/// Registers a container dyld has loaded, with `info` its own __apk_info; loader::load calls it.
/// The entry is never removed. Allocates, so not for signal handlers.
void add(const void *header, const apk_container_info &info, std::string_view name);

/// The container whose stub area holds `pc`, or null. Safe in signal handlers: no locks.
const Container *container_at(std::uint64_t pc);

/// The x18 island of `container` that holds `pc`, or null: the stub area holds thread pointer
/// islands and syscall veneers as well. Safe in signal handlers.
const apk_island *island_at(const Container &container, std::uint64_t pc);

}  // namespace apkrt::islands
