// The environment every guest program starts with (ADR-0039, decision 2): init's global environment,
// which every process on Android inherits from init, with the boot class path of our jars
// (ADR-0033, decision 10). The caller's entries come on top of it.
#pragma once

#include <string>
#include <vector>

namespace apkrt::environment {

/// init's environment with `caller`'s entries ("NAME=value") on top: a name the caller gives keeps
/// init's place with the caller's value, and the caller's other entries follow in its order.
std::vector<std::string> at_start(const std::vector<std::string> &caller);

}  // namespace apkrt::environment
