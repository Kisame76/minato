// Who the guest's process is (ADR-0039 decision 3): every app runs as one app uid, u0_a100, whose gid has the
// same number. One app runs per process and M4 has no IPC, so different uids could not show.
#pragma once

#include <cstdint>

namespace apkrt {

constexpr std::uint32_t app_uid = 10100;  // u0_a100, also as its gid

}  // namespace apkrt
