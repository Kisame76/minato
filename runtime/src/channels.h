// The vsync and input channels (ADR-0036, decision 1): the host's ticks and motion events wait in host memory,
// and each consumer's guest descriptor (Kind::channel) is readable while its channel holds something unread.
// The host's side is apkrt.h; the guest's are the private syscalls of guest_interface.h.
#pragma once

#include "descriptors.h"

#include <cstddef>
#include <cstdint>

namespace apkrt::channels {

/// The private syscalls vsync_connect, vsync_request and vsync_read (guest_interface.h).
long vsync_connect();
long vsync_request(int fd);
long vsync_read(int fd, std::uint64_t vsync);

/// Whether a channel's description (Kind::channel) holds something unread: its POLLIN.
bool readable(descriptors::Description &channel);

/// The open vsync channels, for the tests.
std::size_t vsync_count();

}  // namespace apkrt::channels
