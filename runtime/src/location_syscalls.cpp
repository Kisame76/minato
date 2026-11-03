#include "location_syscalls.h"

#include <cstddef>
#include <limits>
#include <new>

#include "APKRuntime/guest_interface.h"
#include "location_hub.h"
#include "permissions.h"
#include "syscalls.h"

namespace apkrt::location {
namespace {

namespace linux_errno = sys::linux_errno;

/// Whether the host has granted the location group (outcome 1 fine or 2 coarse). Any other stored value — denied (0),
/// unknown (-1) or a nonsense int the host passed — is no grant, so a guest denied in Minato's sheet cannot reach the
/// device by calling the syscall directly (ADR-0063 decision 2, its enforcement).
bool granted() {
    const int outcome = permissions::outcome(APKRT_PERMISSION_LOCATION);
    return outcome == APKRT_PERMISSION_GRANTED || outcome == APKRT_PERMISSION_GRANTED_COARSE;
}

/// Reads `size` bytes from the guest at `address` into `out`; false (answer -EFAULT) on a bad pointer.
bool take(std::uint64_t address, void *out, std::size_t size) {
    return sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(out), address, size);
}

/// Writes `size` bytes to the guest at `address` from `in`; false (answer -EFAULT) on a bad pointer.
bool give(std::uint64_t address, const void *in, std::size_t size) {
    return sys::memory::copy_checked(address, reinterpret_cast<std::uint64_t>(in), size);
}

long do_start(std::uint64_t argument) {
    apkrt_location_control control{};
    if (!take(argument, &control, sizeof control)) return -linux_errno::efault;
    if (control.accuracy != APKRT_LOCATION_ACCURACY_FINE && control.accuracy != APKRT_LOCATION_ACCURACY_COARSE)
        return -linux_errno::einval;
    return start(control.accuracy, control.distance_m);
}

// Copies out a fix (the latest continuous, or the one of `sequence`), always writing the guest buffer so a bad pointer
// is -EFAULT even when there is no fix; returns 1 (filled), 0 (none/gone) or -EFAULT.
long copy_out(std::uint64_t argument, int found, const apkrt_location_fix &fix) {
    if (!give(argument, &fix, sizeof fix)) return -linux_errno::efault;
    return found;
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument, std::uint64_t sequence) try {
    switch (op) {
        case APKRT_LOCATION_START:
            if (!granted()) return -linux_errno::eacces;
            return do_start(argument);
        case APKRT_LOCATION_STOP:
            return stop();  // tearing down is always allowed, even without a grant
        case APKRT_LOCATION_CURRENT:
            if (!granted()) return -linux_errno::eacces;
            if (argument == 0) return -linux_errno::einval;
            return request_single(argument);
        case APKRT_LOCATION_CANCEL:
            if (argument == 0) return -linux_errno::einval;
            return cancel_single(argument);  // tearing down is always allowed
        case APKRT_LOCATION_LAST: {
            if (!granted()) return -linux_errno::eacces;
            apkrt_location_fix fix{};
            const int found = last(fix);
            return copy_out(argument, found, fix);
        }
        case APKRT_LOCATION_GET: {
            if (!granted()) return -linux_errno::eacces;
            apkrt_location_fix fix{};
            const int found = get(sequence, fix);
            return copy_out(argument, found, fix);
        }
        case APKRT_LOCATION_PERMISSION: {
            // A permission read (checkSelfPermission) for any group; never gated, since it is how a guest learns it was
            // denied. `argument` is the group. The stored int is normalized to {0,1,2} by the same rule the gate uses,
            // so the read and the gate always agree: anything the host stored that is not fine or coarse reads denied.
            if (argument > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                return -linux_errno::einval;  // checked before the cast, so a high bit cannot alias a known group
            const auto group = static_cast<std::int32_t>(argument);
            if (!permissions::known_group(group)) return -linux_errno::einval;
            const int outcome = permissions::outcome(group);
            return (outcome == APKRT_PERMISSION_GRANTED || outcome == APKRT_PERMISSION_GRANTED_COARSE)
                       ? outcome
                       : APKRT_PERMISSION_DENIED;
        }
        default:
            return -linux_errno::einval;
    }
} catch (const std::bad_alloc &) {
    return -linux_errno::enomem;  // a growing one-shot table ran out of memory: never unwind out of the dispatcher
}

}  // namespace apkrt::location
