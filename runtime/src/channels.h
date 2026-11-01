// The vsync and input channels (ADR-0036, decision 1): the host's ticks and motion events wait in host memory,
// and each consumer's guest descriptor (Kind::channel) is readable while its channel holds something unread.
// The host's side is apkrt.h; the guest's are the private syscalls of guest_interface.h.
#pragma once

#include "descriptors.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace apkrt::channels {

/// The private syscalls vsync_connect, vsync_request and vsync_read (guest_interface.h).
long vsync_connect();
long vsync_request(int fd);
long vsync_read(int fd, std::uint64_t vsync);

/// The private syscalls input_connect and input_read (guest_interface.h).
long input_connect(std::uint64_t id);
long input_read(int fd, std::uint64_t event);

/// Whether the description is a WebView channel, whose records the guest reads with read() (ADR-0056 step a); the
/// other channels have only their private syscalls.
bool reads_records(descriptors::Description &channel);

/// Whether a channel's description (Kind::channel) holds something unread: its POLLIN.
bool readable(descriptors::Description &channel);

/// The open vsync channels, for the tests.
std::size_t vsync_count();

// The WebView channel (ADR-0056 step a): host -> guest records over an AF_UNIX/SOCK_STREAM pair whose guest end is the
// description's host descriptor. The host frames and queues records with webview_post, whose writer thread never blocks
// the caller (ADR-0056 decision 3); the guest reads whole records with the header-only WebViewReader
// (APKRuntime/apkrt_webview_reader.h), so the one parser links no runtime and is tested once.

/// Opens a WebView channel for a view `token` (not 0) and returns its guest fd, or a negative Linux errno.
long webview_make(std::uint64_t token);

/// Frames `payload` (`length` bytes) as record(s) of `type` for the view's channel -- split at
/// APKRT_WEBVIEW_RECORD_MAX with APKRT_WEBVIEW_MORE on all but the last -- and queues them for its writer thread.
/// 0, or a negative Linux errno (-ENOENT for a token with no channel). This stands in for the host until step c).
long webview_post(std::uint64_t token, std::uint32_t type, std::uint64_t request_id, const void *payload,
                  std::size_t length);

/// For the tests: queues `length` raw bytes onto the view's channel, bypassing framing, so a test can deliver a
/// record in pieces or a malformed header. 0, or a negative Linux errno.
long webview_post_raw(std::uint64_t token, const void *bytes, std::size_t length);

/// Closes the view's channel: the guest's reader sees EOF once it has read what was queued. 0, or a negative errno.
long webview_close(std::uint64_t token);

}  // namespace apkrt::channels
