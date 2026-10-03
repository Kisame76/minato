#include "host_calls.h"

#include "APKRuntime/apkrt.h"
#include "signals.h"
#include "syscall_frame.h"
#include "threads.h"

#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <mutex>

namespace apkrt::host_calls {
namespace {

constexpr long enoent = 2, einval = 22;
constexpr std::size_t name_capacity = 64;  // with its NUL

struct Provider {
    char name[name_capacity];
    apkrt_host_provider function;
    void *context;
};

constexpr std::size_t provider_capacity = 8;
std::mutex providers_mutex;
Provider providers[provider_capacity];
std::size_t provider_count = 0;

/// The length of `name`, or name_capacity when it has no NUL within it.
std::size_t name_length(const char *name) {
    return ::strnlen(name, name_capacity);
}

}  // namespace

long bind_host_table(std::uint64_t name_address, std::uint64_t table_address, std::uint64_t length, std::uint64_t hash) {
    const auto *name = reinterpret_cast<const char *>(name_address);
    if (name == nullptr || table_address == 0 || length == 0 || name_length(name) == name_capacity) return -einval;
    Provider provider{};
    {
        const std::lock_guard lock(providers_mutex);
        std::size_t index = 0;
        while (index < provider_count && std::strcmp(providers[index].name, name) != 0) ++index;
        if (index == provider_count) return -enoent;
        provider = providers[index];
    }
    auto **table = reinterpret_cast<void **>(table_address);
    // The provider opens ANGLE or MoltenVK: host code, which no runtime signal may interrupt.
    int answer;
    if (threads::current() != nullptr) {
        auto &state = signals::current();
        const auto side = state.in_runtime.load();
        signals::enter_host_code(state);
        answer = provider.function(provider.context, table, length, hash);
        signals::leave_host_code(state, side);
    } else {
        answer = provider.function(provider.context, table, length, hash);
    }
    if (answer != 0) return -einval;
    table[0] = reinterpret_cast<void *>(&apkrt_host_call);
    return 0;
}

bool has_provider(const char *name) {
    const std::lock_guard lock(providers_mutex);
    for (std::size_t index = 0; index < provider_count; ++index) {
        if (std::strcmp(providers[index].name, name) == 0) return true;
    }
    return false;
}

}  // namespace apkrt::host_calls

extern "C" int apkrt_is_guest_thread(void) {
    return apkrt::threads::current() != nullptr ? 1 : 0;
}

extern "C" int apkrt_set_host_provider(const char *name, apkrt_host_provider provider, void *context) {
    using namespace apkrt::host_calls;
    if (name == nullptr) return -1;
    const auto length = name_length(name);
    if (length == 0 || length == name_capacity) return -1;
    const std::lock_guard lock(providers_mutex);
    std::size_t index = 0;
    while (index < provider_count && std::strcmp(providers[index].name, name) != 0) ++index;
    if (provider == nullptr) {
        if (index == provider_count) return 0;
        for (std::size_t next = index + 1; next < provider_count; ++next) providers[next - 1] = providers[next];
        --provider_count;
        return 0;
    }
    if (index == provider_count) {
        if (provider_count == provider_capacity) return -1;
        ++provider_count;
    }
    auto &entry = providers[index];
    std::memcpy(entry.name, name, length + 1);
    entry.function = provider;
    entry.context = context;
    return 0;
}

/// apkrt_host_call, before it pushes a call onto a stack of host calls that is not empty (ADR-0030,
/// decision 1): drops the entries that a longjmp out of a callback left behind. Those are the entries
/// on top made at or below `stack_pointer` on the same stack, where the guest's alternate stack counts
/// as another one: a signal handler there runs inside the calls below it, whatever its addresses.
extern "C" void apkrt_drop_stale_host_calls(apkrt::signals::ThreadSignals *state, std::uint64_t stack_pointer) {
    const auto alternate = state->alternate_stack;
    const auto on_alternate = [&](std::uint64_t sp) {
        return (alternate.flags & apkrt::signals::ss_disable) == 0 && alternate.size != 0 && sp > alternate.sp &&
               sp - alternate.sp <= alternate.size;
    };
    const bool here = on_alternate(stack_pointer);
    while (state->host_calls > 0) {
        const auto made = state->host_call_stack[state->host_calls - 1].stack_pointer;
        if (made > stack_pointer || on_alternate(made) != here) break;
        --state->host_calls;
    }
}

/// apkrt_host_call, when its stack of host calls is full (ADR-0030, decision 1): in host code, so
/// the trap is a fault of the runtime.
extern "C" [[noreturn]] void apkrt_host_calls_too_deep(void) {
    static const char message[] = "apkrt: host calls nest deeper than 16 (ADR-0030)\n";
    static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
    __builtin_trap();
}
