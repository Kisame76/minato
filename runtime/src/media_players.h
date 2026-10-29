// The guest's media players (ADR-0060 e2e1): the process's players by the runtime's IDs, each with the pipe of its
// records; the host's players (apkrt.h's apkrt_media_players) play.
#pragma once

#include <cstdint>

namespace apkrt::media_players {

/// media_player (guest_interface.h): `op` with the address of a struct apkrt_media_player.
long call(std::uint64_t op, std::uint64_t argument);

}  // namespace apkrt::media_players
