// The runtime's calls into guest code (ADR-0025): constructors, ifunc resolvers and the functions
// libc.so has for the runtime. They go through apkrt_call_guest (syscall_entry.S), so that the
// thread counts as in guest code while one runs and as in the runtime again once it has returned.
#pragma once

#include "syscall_frame.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace apkrt {

/// A type that travels in a general-purpose register the way AAPCS64 passes it: an integer or a
/// pointer. Not bool, whose upper bits a callee may leave undefined; no floating point.
template <typename T>
inline constexpr bool in_x_register = std::is_pointer_v<T> || (std::is_integral_v<T> && !std::is_same_v<T, bool>);

/// Calls the guest function `function` with at most three integer or pointer arguments, each
/// converted to its parameter's type first.
template <typename Result, typename... Parameters, typename... Arguments>
Result call_guest(Result (*function)(Parameters...), Arguments... arguments) {
    static_assert(sizeof...(Parameters) <= 3 && sizeof...(Parameters) == sizeof...(Arguments));
    static_assert((in_x_register<Parameters> && ...) && (std::is_void_v<Result> || in_x_register<Result>));
    const auto word = [](auto value) -> std::uint64_t {
        if constexpr (std::is_pointer_v<decltype(value)>) {
            return reinterpret_cast<std::uint64_t>(value);
        } else {
            return static_cast<std::uint64_t>(value);
        }
    };
    std::uint64_t words[3] = {};
    [[maybe_unused]] std::size_t index = 0;
    ((words[index++] = word(static_cast<Parameters>(arguments))), ...);
    const auto result = apkrt_call_guest(reinterpret_cast<std::uint64_t>(function), words[0], words[1], words[2]);
    if constexpr (std::is_void_v<Result>) {
        static_cast<void>(result);
    } else if constexpr (std::is_pointer_v<Result>) {
        return reinterpret_cast<Result>(result);
    } else {
        return static_cast<Result>(result);
    }
}

}  // namespace apkrt
