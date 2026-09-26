#include "session.h"

#include <atomic>

namespace apkrt::session {
namespace {

std::atomic_flag claimed = ATOMIC_FLAG_INIT;  // by the first apkrt_session_begin that is valid
apkrt_session described{};                    // written once, before `begun`
std::atomic<bool> begun{false};

}  // namespace

EndHandler end_handler() {
    if (!begun.load(std::memory_order_acquire)) return {nullptr, nullptr};
    return {described.end, described.context};
}

}  // namespace apkrt::session

extern "C" int apkrt_session_begin(const struct apkrt_session *session) {
    using namespace apkrt::session;
    if (session == nullptr || session->end == nullptr) return -1;
    if (claimed.test_and_set()) return -1;
    described = *session;
    begun.store(true, std::memory_order_release);
    return 0;
}
