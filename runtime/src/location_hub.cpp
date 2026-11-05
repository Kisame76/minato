#include "location_hub.h"

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <random>
#include <vector>

#include "APKRuntime/apkrt.h"
#include "location_fudge.h"
#include "permissions.h"

namespace apkrt::location {

// The guest and the host share this layout; a silent change of its size would desynchronize the two ABIs.
static_assert(sizeof(apkrt_location_fix) == 72, "apkrt_location_fix is a 72-byte ABI");

namespace {

[[clang::no_destroy]] constinit std::mutex mutex;  // constant-init + immortal (host push, network.cpp); guards below

// Serializes the host's start/stop/request/cancel calls so their order matches the order the ops decided them in, and
// keeps them off `mutex` so a future blocking host call cannot deadlock a push (the sensor hub's out-of-order finding).
// An op takes this first, then `mutex` for the decision, drops `mutex`, and makes the host call still holding this.
// apkrt_location_push / _push_single take only `mutex`, so a host call cannot deadlock them. Never held with `mutex`
// taken around it except inside an op's own decision block.
[[clang::no_destroy]] constinit std::mutex notify_mutex;  // constant-init + immortal (host push, network.cpp)

apkrt_location_host registered{};
bool have_host = false;

apkrt_location_fix continuous{};  // the latest continuous fix
bool have_continuous = false;
std::uint64_t continuous_seq = 0;
std::uint64_t next_seq = 1;

// A one-shot (getCurrentLocation): outstanding after a request, resolved-with-fix after the host answers (kept until
// its GET or CANCEL), and gone after a no-fix answer, a GET or a CANCEL. Keyed by the guest's token.
struct OneShot {
    std::uint64_t token;
    bool resolved;        // the host answered with a fix
    std::uint64_t seq;    // the fix's sequence, once resolved
    apkrt_location_fix fix;
};
std::vector<OneShot> one_shots;

location::Fudger fudger;
bool fudger_seeded = false;

OneShot *find_one_shot(std::uint64_t token) {
    for (auto &entry : one_shots) {
        if (entry.token == token) return &entry;
    }
    return nullptr;
}

// Whether the host has granted location (outcome 1 fine or 2 coarse). Read fresh at every delivery so the delivery
// path fails closed if the host revokes the grant in the window between the syscall's gate check and the hand-out: a
// just-denied guest then gets no fix, not even a coarse one (defense in depth matching the syscall gate).
bool has_grant() {
    const int outcome = permissions::outcome(APKRT_PERMISSION_LOCATION);
    return outcome == APKRT_PERMISSION_GRANTED || outcome == APKRT_PERMISSION_GRANTED_COARSE;
}

// A coarse grant (a grant that is not fine) means the guest gets a fudged, reduced-accuracy fix; read fresh each time,
// so a grant the host downgrades fine→coarse between a push and the GET still coarsens the fix the guest finally reads.
// Only consulted on a path already guarded by has_grant(), so "not fine" there means exactly the coarse grant.
bool coarse_grant() { return permissions::outcome(APKRT_PERMISSION_LOCATION) != APKRT_PERMISSION_GRANTED; }

// The accuracy to ask the host for: the guest's wish, forced to coarse under a coarse grant. Caller holds nothing
// special (reads the grant store, which has its own lock).
std::int32_t effective_accuracy(std::int32_t wanted) {
    return permissions::outcome(APKRT_PERMISSION_LOCATION) == APKRT_PERMISSION_GRANTED_COARSE
               ? APKRT_LOCATION_ACCURACY_COARSE
               : wanted;
}

// Fills `out` from `raw`, fudged to the coarse grid when the grant is not fine (caller holds `mutex`). A coarse fix
// keeps only latitude, longitude, time, provider and the coarse horizontal accuracy, as Android's coarse Location does.
void deliver(const apkrt_location_fix &raw, apkrt_location_fix &out) {
    if (!coarse_grant()) {
        out = raw;
        return;
    }
    if (!fudger_seeded) {
        std::random_device source;
        fudger.reseed((static_cast<std::uint64_t>(source()) << 32) | source());
        fudger_seeded = true;
    }
    const FudgedFix fudged = fudger.fudge(raw.latitude, raw.longitude, raw.elapsed_realtime_ns);
    out = {};
    out.latitude = fudged.latitude;
    out.longitude = fudged.longitude;
    out.horizontal_accuracy_m = fudged.accuracy_m;
    out.time_ms = raw.time_ms;
    out.elapsed_realtime_ns = raw.elapsed_realtime_ns;
    out.provider = raw.provider;
    out.valid = APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY;
}

std::uint32_t coarse_flag() { return coarse_grant() ? APKRT_LOCATION_EVENT_COARSE : 0u; }

// A fix the host sent is usable only with a non-negative horizontal accuracy (a negative one is CLLocation's "invalid",
// and the test also catches NaN, since NaN >= 0 is false).
bool usable(const apkrt_location_fix &fix) { return fix.horizontal_accuracy_m >= 0.0f; }

}  // namespace

int start(std::int32_t accuracy, std::int32_t distance_m) {
    const std::lock_guard<std::mutex> serialize(notify_mutex);
    const std::int32_t effective = effective_accuracy(accuracy);
    bool present;
    apkrt_location_host host;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        present = have_host;
        host = registered;
    }
    if (present && host.start != nullptr) host.start(host.context, effective, distance_m);
    return 0;
}

int stop() {
    const std::lock_guard<std::mutex> serialize(notify_mutex);
    bool present;
    apkrt_location_host host;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        present = have_host;
        host = registered;
    }
    if (present && host.stop != nullptr) host.stop(host.context);
    return 0;
}

int request_single(std::uint64_t token) {
    const std::lock_guard<std::mutex> serialize(notify_mutex);
    bool present;
    apkrt_location_host host;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        if (find_one_shot(token) != nullptr) return -EEXIST;
        if (one_shots.size() >= APKRT_LOCATION_ONESHOT_MAX) return -EBUSY;
        one_shots.push_back({token, false, 0, {}});
        present = have_host;
        host = registered;
    }
    if (present && host.request_single != nullptr) host.request_single(host.context, token);
    return 0;
}

int cancel_single(std::uint64_t token) {
    const std::lock_guard<std::mutex> serialize(notify_mutex);
    bool present;
    apkrt_location_host host;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        auto at = std::find_if(one_shots.begin(), one_shots.end(), [&](auto &e) { return e.token == token; });
        if (at == one_shots.end()) return -ENOENT;
        one_shots.erase(at);
        present = have_host;
        host = registered;
    }
    if (present && host.cancel_single != nullptr) host.cancel_single(host.context, token);
    return 0;
}

int last(apkrt_location_fix &out) {
    const std::lock_guard<std::mutex> hold(mutex);
    if (!has_grant()) return 0;  // fail closed: the grant was revoked after the syscall's gate let this through
    if (!have_continuous) return 0;
    deliver(continuous, out);
    return 1;
}

int get(std::uint64_t seq, apkrt_location_fix &out) {
    const std::lock_guard<std::mutex> hold(mutex);
    if (!has_grant()) return 0;  // fail closed: the grant was revoked after the syscall's gate let this through
    if (have_continuous && seq == continuous_seq) {
        deliver(continuous, out);
        return 1;
    }
    for (auto at = one_shots.begin(); at != one_shots.end(); ++at) {
        if (at->resolved && at->seq == seq) {
            deliver(at->fix, out);
            one_shots.erase(at);  // a one-shot fix is dropped once it is fetched
            return 1;
        }
    }
    return 0;  // gone: a newer continuous fix replaced it, or it was already fetched or cancelled
}

std::uint64_t push_continuous(const apkrt_location_fix &fix) {
    const std::lock_guard<std::mutex> hold(mutex);
    if (!usable(fix)) return 0;
    continuous = fix;
    have_continuous = true;
    continuous_seq = next_seq++;
    apkrt_session_post(APKRT_EVENT_LOCATION, continuous_seq, coarse_flag(), 0);
    return continuous_seq;
}

std::uint64_t push_single(std::uint64_t token, const apkrt_location_fix *fix) {
    const std::lock_guard<std::mutex> hold(mutex);
    OneShot *entry = find_one_shot(token);
    if (entry == nullptr || entry->resolved) return 0;  // unknown or already resolved: ignore
    if (fix == nullptr || !usable(*fix)) {
        // No location: announce a no-fix one-shot (no GET follows) and drop the entry at once.
        apkrt_session_post(APKRT_EVENT_LOCATION, 0, APKRT_LOCATION_EVENT_ONE_SHOT | APKRT_LOCATION_EVENT_NO_FIX, token);
        one_shots.erase(std::remove_if(one_shots.begin(), one_shots.end(), [&](auto &e) { return e.token == token; }),
                        one_shots.end());
        return 0;
    }
    entry->resolved = true;
    entry->seq = next_seq++;
    entry->fix = *fix;
    apkrt_session_post(APKRT_EVENT_LOCATION, entry->seq, APKRT_LOCATION_EVENT_ONE_SHOT | coarse_flag(), token);
    return entry->seq;
}

void reset() {
    const std::lock_guard<std::mutex> hold(mutex);
    registered = {};
    have_host = false;
    continuous = {};
    have_continuous = false;
    continuous_seq = 0;
    next_seq = 1;
    one_shots.clear();
    fudger_seeded = false;
}

}  // namespace apkrt::location

extern "C" {

void apkrt_location_set_host(const apkrt_location_host *host) {
    const std::lock_guard<std::mutex> hold(apkrt::location::mutex);
    if (host == nullptr) {
        apkrt::location::registered = {};
        apkrt::location::have_host = false;
        return;
    }
    apkrt::location::registered = *host;
    apkrt::location::have_host = true;
}

void apkrt_location_push(const apkrt_location_fix *fix) {
    if (fix == nullptr) return;
    apkrt::location::push_continuous(*fix);
}

void apkrt_location_push_single(uint64_t token, const apkrt_location_fix *fix) {
    apkrt::location::push_single(token, fix);
}

}  // extern "C"
