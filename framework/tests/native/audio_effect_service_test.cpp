// The local effect service of android.media.audiofx (framework/native/android_runtime/audio_effect_service.h) on the
// host: the list of effects, control passing between instances of one effect on a session, the enabled state, and the
// parameters of the Equalizer, Dynamic Bass Boost, Virtualizer, Loudness Enhancer and the two reverbs, in the byte
// protocol of Android's effects, with the values AVD measured (fixtures/expected/audiofx-test.logcat).
//   audio_effect_service_test   (built and run by tools/framework-host-check.sh with the host's c++)
#include "../../native/android_runtime/audio_effect_service.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace minato::audiofx;

namespace {

int failures = 0;

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            ++failures;                                                            \
        }                                                                          \
    } while (0)

const char *kEqualizer = "0bed4300-ddd6-11db-8f34-0002a5d5c51b";
const char *kEqualizerImpl = "ce772f20-847d-11df-bb17-0002a5d5c51b";
const char *kBassBoost = "0634f220-ddd4-11db-a0fc-0002a5d5c51b";
const char *kVirtualizer = "37cc2c00-dddd-11db-8577-0002a5d5c51b";
const char *kLoudness = "fe3199be-aed0-413f-87bb-11260eb63cf1";
const char *kPresetReverb = "47382d60-ddd8-11db-bf3a-0002a5d5c51b";
const char *kEnvReverb = "c2e5d5f0-94bd-4763-9cac-4e234d06839e";
const char *kNull = "ec7178ec-e5e1-4432-a3f4-4657e6795210";

std::vector<uint8_t> param(int32_t id) {
    std::vector<uint8_t> bytes;
    write_int(bytes, id);
    return bytes;
}

std::vector<uint8_t> param(int32_t id, int32_t arg) {
    std::vector<uint8_t> bytes = param(id);
    write_int(bytes, arg);
    return bytes;
}

struct Events {
    std::vector<std::pair<int, bool>> seen;
};

void listen(void *context, int id, bool has_control) {
    static_cast<Events *>(context)->seen.push_back({id, has_control});
}

int create(Service &service, const char *type, int session, int priority = 0, Events *events = nullptr,
           int *id = nullptr) {
    int made = 0;
    const Entry *entry = nullptr;
    const int status =
        service.create(type, kNull, priority, session, &made, &entry, events != nullptr ? listen : nullptr, events);
    if (id != nullptr) *id = made;
    return status;
}

int16_t short_of(const std::vector<uint8_t> &bytes, size_t at = 0) {
    int16_t value;
    std::memcpy(&value, bytes.data() + at, 2);
    return value;
}

int get(Service &service, int id, const std::vector<uint8_t> &p, std::vector<uint8_t> &out, int vsize = 64) {
    out.assign(static_cast<size_t>(vsize), 0);
    const int status = service.get_parameter(id, p.data(), static_cast<int>(p.size()), out.data(), vsize);
    if (status > 0) out.resize(static_cast<size_t>(status));
    return status;
}

int set_short(Service &service, int id, const std::vector<uint8_t> &p, int16_t value) {
    uint8_t bytes[2];
    std::memcpy(bytes, &value, 2);
    return service.set_parameter(id, p.data(), static_cast<int>(p.size()), bytes, 2);
}

void the_list_is_the_emulators() {
    CHECK(entries().size() == 13);
    CHECK(std::string(entries()[10].descriptor.name) == "Equalizer");
    CHECK(Service::find(kEqualizer, kNull) != nullptr && Service::find(kEqualizer, kNull)->kind == Kind::equalizer);
    CHECK(Service::find("", kEqualizerImpl) != nullptr);
    CHECK(Service::find(kNull, kNull) != nullptr);  // any effect
    CHECK(Service::find("7b491460-8d4d-11e0-bd61-0002a5d5c51b", kNull) == nullptr);  // no echo canceler in the list
    Service service;
    int id = 0;
    CHECK(create(service, "58b4b260-8e06-11e0-aa8e-0002a5d5c51b", 1, 0, nullptr, &id) == kBadValue);
}

void control_passes_between_instances_of_one_effect_on_a_session() {
    Service service;
    Events first_events, second_events;
    int first = 0, second = 0, other_session = 0, other_effect = 0;
    CHECK(create(service, kEqualizer, 5, 0, &first_events, &first) == kSuccess);
    CHECK(service.get(first)->has_control);
    CHECK(create(service, kEqualizer, 5, 0, &second_events, &second) == kSuccess);  // the newest takes control
    CHECK(!service.get(first)->has_control && service.get(second)->has_control);
    CHECK(first_events.seen.size() == 1 && first_events.seen[0] == std::make_pair(first, false));
    CHECK(second_events.seen.empty());  // its own gain is its creation
    CHECK(create(service, kEqualizer, 6, 0, nullptr, &other_session) == kSuccess);  // another session
    CHECK(create(service, kBassBoost, 5, 0, nullptr, &other_effect) == kSuccess);   // another effect
    CHECK(service.get(first)->has_control == false);
    service.release(second);
    CHECK(service.get(first)->has_control);
    CHECK(first_events.seen.size() == 2 && first_events.seen[1] == std::make_pair(first, true));
    int low = 0;
    CHECK(create(service, kEqualizer, 5, -1, nullptr, &low) == kAlreadyExists);  // a lower priority does not take it
    CHECK(!service.get(low)->has_control && service.get(first)->has_control);
    int high = 0;
    CHECK(create(service, kEqualizer, 5, 3, nullptr, &high) == kSuccess);
    CHECK(service.get(high)->has_control && !service.get(first)->has_control);
}

void enabling_needs_control_and_a_live_instance() {
    Service service;
    int first = 0, second = 0;
    create(service, kEqualizer, 1, 0, nullptr, &first);
    CHECK(service.set_enabled(first, true) == kSuccess && service.get(first)->enabled);
    create(service, kEqualizer, 1, 0, nullptr, &second);
    CHECK(service.set_enabled(first, false) == kInvalidOperation && service.get(first)->enabled);
    service.release(first);
    CHECK(service.set_enabled(first, true) == kNoInit);
    std::vector<uint8_t> out;
    CHECK(get(service, first, param(0), out) == kNoInit);
}

void the_equalizer_answers_with_the_emulators_values() {
    Service service;
    int id = 0;
    create(service, kEqualizer, 1, 0, nullptr, &id);
    std::vector<uint8_t> out;
    CHECK(get(service, id, param(0), out) == 2 && short_of(out) == 5);
    CHECK(get(service, id, param(1), out) == 4 && short_of(out) == -1500 && short_of(out, 2) == 1500);
    CHECK(get(service, id, param(2, 0), out) == 2 && short_of(out) == 300);
    CHECK(get(service, id, param(2, 4), out) == 2 && short_of(out) == 300);
    CHECK(get(service, id, param(2, 1), out) == 2 && short_of(out) == 0);
    CHECK(get(service, id, param(2, 5), out) == kBadValue);
    CHECK(get(service, id, param(3, 0), out) == 4 && read_int(out.data()) == 60000);
    CHECK(get(service, id, param(3, 4), out) == 4 && read_int(out.data()) == 14000000);
    CHECK(get(service, id, param(4, 1), out) == 8 && read_int(out.data()) == 120001 &&
          read_int(out.data(), 4) == 460000);
    CHECK(get(service, id, param(5, 0), out) == 2 && short_of(out) == -22);
    CHECK(get(service, id, param(5, 1000000), out) == 2 && short_of(out) == 2);
    CHECK(get(service, id, param(6), out) == 2 && short_of(out) == 0);
    CHECK(get(service, id, param(7), out) == 2 && short_of(out) == 10);
    CHECK(get(service, id, param(8, 5), out) > 0 && std::string(reinterpret_cast<char *>(out.data())) == "Heavy Metal");
    CHECK(get(service, id, param(8, 10), out) == kBadValue);
    CHECK(get(service, id, param(9), out) == 14 && short_of(out) == 0 && short_of(out, 2) == 5 &&
          short_of(out, 4) == 300);
    CHECK(get(service, id, param(2, 0), out, 1) == kBadValue);  // a value buffer too small
}

void the_equalizer_stores_levels_and_presets() {
    Service service;
    int id = 0;
    create(service, kEqualizer, 1, 0, nullptr, &id);
    std::vector<uint8_t> out;
    CHECK(set_short(service, id, param(2, 0), 1500) == kSuccess);
    CHECK(get(service, id, param(2, 0), out) == 2 && short_of(out) == 1500);
    CHECK(get(service, id, param(6), out) == 2 && short_of(out) == -1);  // a band changed by hand: no preset
    CHECK(set_short(service, id, param(2, 2), 1600) == kSuccess);  // beyond the range, kept as Android's does
    CHECK(get(service, id, param(2, 2), out) == 2 && short_of(out) == 1600);
    CHECK(set_short(service, id, param(2, 9), 0) == kBadValue);
    CHECK(set_short(service, id, param(6), 2) == kSuccess);  // Dance: 6, 0, 2, 4, 1 dB
    int16_t dance[5] = {600, 0, 200, 400, 100};
    for (int band = 0; band < 5; ++band) {
        CHECK(get(service, id, param(2, band), out) == 2 && short_of(out) == dance[band]);
    }
    CHECK(get(service, id, param(6), out) == 2 && short_of(out) == 2);
    CHECK(set_short(service, id, param(6), 10) == kBadValue);
    CHECK(set_short(service, id, param(6), -1) == kBadValue);
    // properties: preset, bands, five levels
    std::vector<uint8_t> properties;
    write_short(properties, 3);
    write_short(properties, 5);
    for (int16_t level : {0, 100, -100, 50, 0}) write_short(properties, level);
    CHECK(service.set_parameter(id, param(9).data(), 4, properties.data(), static_cast<int>(properties.size())) ==
          kSuccess);
    CHECK(get(service, id, param(2, 2), out) == 2 && short_of(out) == -100);
    CHECK(get(service, id, param(6), out) == 2 && short_of(out) == 3);
    // a second instance has no control and cannot set
    int second = 0;
    create(service, kEqualizer, 1, -1, nullptr, &second);
    CHECK(set_short(service, second, param(2, 0), 0) == kInvalidOperation);
}

void the_other_effects_store_and_clamp() {
    Service service;
    int bass = 0, virt = 0, loud = 0, preset = 0, env = 0;
    create(service, kBassBoost, 1, 0, nullptr, &bass);
    create(service, kVirtualizer, 1, 0, nullptr, &virt);
    create(service, kLoudness, 1, 0, nullptr, &loud);
    create(service, kPresetReverb, 1, 0, nullptr, &preset);
    create(service, kEnvReverb, 1, 0, nullptr, &env);
    std::vector<uint8_t> out;
    for (int id : {bass, virt}) {
        CHECK(get(service, id, param(0), out) == 4 && read_int(out.data()) == 1);
        CHECK(get(service, id, param(1), out) == 2 && short_of(out) == 0);
        CHECK(set_short(service, id, param(1), 500) == kSuccess);
        CHECK(get(service, id, param(1), out) == 2 && short_of(out) == 500);
        CHECK(set_short(service, id, param(1), 2000) == kSuccess);  // accepted and kept, as Android's
        CHECK(get(service, id, param(1), out) == 2 && short_of(out) == 2000);
    }
    CHECK(get(service, virt, param(4), out) == 4 && read_int(out.data()) == 0);
    CHECK(get(service, bass, param(4), out) == kBadValue);
    int32_t gain = 500;
    CHECK(service.set_parameter(loud, param(0).data(), 4, reinterpret_cast<uint8_t *>(&gain), 4) == kSuccess);
    CHECK(get(service, loud, param(0), out) == 4 && read_int(out.data()) == 500);
    CHECK(get(service, preset, param(0), out) == 2 && short_of(out) == 0);
    CHECK(set_short(service, preset, param(0), 5) == kSuccess);
    CHECK(get(service, preset, param(0), out) == 2 && short_of(out) == 5);
    CHECK(set_short(service, preset, param(0), 7) == kBadValue);
    CHECK(get(service, env, param(2), out) == 4 && read_int(out.data()) == 1490);
    CHECK(get(service, env, param(3), out) == 2 && short_of(out) == 420);
    CHECK(get(service, env, param(4), out) == 2 && short_of(out) == 0 && get(service, env, param(5), out) == 4 &&
          read_int(out.data()) == 0);
    CHECK(get(service, env, param(6), out) == 2 && short_of(out) == -6000 && get(service, env, param(7), out) == 4 &&
          read_int(out.data()) == 0);
    int32_t decay = 3000;
    CHECK(service.set_parameter(env, param(2).data(), 4, reinterpret_cast<uint8_t *>(&decay), 4) == kSuccess);
    CHECK(get(service, env, param(2), out) == 4 && read_int(out.data()) == 3000);
    CHECK(get(service, env, param(77), out) == kBadValue);
}

void the_applied_settings_of_a_session_are_those_of_the_enabled_instances_in_control() {
    Service service;
    int eq = 0, eq_loser = 0, loud = 0, boost = 0, other = 0;
    CHECK(create(service, kEqualizer, 5, 0, nullptr, &eq) == kSuccess);
    CHECK(create(service, kLoudness, 5, 0, nullptr, &loud) == kSuccess);
    CHECK(create(service, kBassBoost, 5, 0, nullptr, &boost) == kSuccess);
    CHECK(create(service, kLoudness, 6, 0, nullptr, &other) == kSuccess);
    Applied none = service.applied(5);
    CHECK(!none.equalizer && !none.bass_boost && !none.virtualizer && !none.loudness);  // created, not enabled
    CHECK(service.set_enabled(eq, true) == kSuccess && service.set_enabled(loud, true) == kSuccess);
    CHECK(set_short(service, eq, param(2, 2), 1500) == kSuccess);
    int32_t gain = 600;
    CHECK(service.set_parameter(loud, param(0).data(), 4, reinterpret_cast<uint8_t *>(&gain), 4) == kSuccess);
    Applied applied = service.applied(5);
    CHECK(applied.equalizer && applied.bands[2] == 1500 && applied.bands[0] == 300 && applied.bands[4] == 300);
    CHECK(applied.loudness && applied.loudness_millibel == 600 && !applied.bass_boost);
    CHECK(!service.applied(6).loudness && !service.applied(0).equalizer);  // another session, the whole mix
    // A bass boost takes its strength; the session of another instance does not leak.
    CHECK(service.set_enabled(boost, true) == kSuccess && set_short(service, boost, param(1), 700) == kSuccess);
    CHECK(service.applied(5).bass_boost && service.applied(5).bass_strength == 700);
    // A second equalizer with higher priority has control; the first no longer counts, and its enabled flag with it.
    CHECK(create(service, kEqualizer, 5, 10, nullptr, &eq_loser) == kSuccess);
    CHECK(!service.applied(5).equalizer);  // the new one is not enabled
    CHECK(service.set_enabled(eq_loser, true) == kSuccess);
    CHECK(service.applied(5).equalizer && service.applied(5).bands[2] == 0);  // its own levels: the defaults
    service.release(eq_loser);  // control returns to the first, still enabled
    CHECK(service.applied(5).equalizer && service.applied(5).bands[2] == 1500);
    service.release(eq);
    CHECK(!service.applied(5).equalizer);
    CHECK(service.set_enabled(boost, false) == kSuccess && !service.applied(5).bass_boost);
}

void the_reverbs_are_applied_as_the_environmental_values_of_the_preset_or_the_environment() {
    Service service;
    int preset = 0, env = 0;
    CHECK(create(service, kPresetReverb, 5, 0, nullptr, &preset) == kSuccess);
    CHECK(!service.applied(5).reverb);  // created, not enabled
    CHECK(service.set_enabled(preset, true) == kSuccess);
    CHECK(!service.applied(5).reverb);  // preset 0 is none: nothing to play
    CHECK(set_short(service, preset, param(0), 3) == kSuccess);  // large room
    Applied applied = service.applied(5);
    const int32_t large_room[7] = {-400, -600, 1500, 830, -1000, 1000, 1000};  // EffectReverb.cpp's row
    CHECK(applied.reverb);
    for (int i = 0; i < 7; ++i) CHECK(applied.reverb_params[i] == large_room[i]);
    CHECK(set_short(service, preset, param(0), 6) == kSuccess);  // plate
    applied = service.applied(5);
    const int32_t plate[7] = {-400, -200, 1300, 900, 0, 1000, 750};
    for (int i = 0; i < 7; ++i) CHECK(applied.reverb_params[i] == plate[i]);
    CHECK(create(service, kEnvReverb, 5, 0, nullptr, &env) == kSuccess);
    CHECK(service.set_enabled(env, true) == kSuccess);  // the environment, once on, is the one that plays
    applied = service.applied(5);
    const int32_t defaults[7] = {-6000, 0, 1490, 420, -6000, 1000, 1000};
    CHECK(applied.reverb);
    for (int i = 0; i < 7; ++i) CHECK(applied.reverb_params[i] == defaults[i]);
    CHECK(service.set_enabled(env, false) == kSuccess && service.applied(5).reverb_params[2] == 1300);
    CHECK(!service.applied(6).reverb);
}

void a_probe_answers_as_a_create_would_without_making_an_instance() {
    Service service;
    Events low_events;
    int low = 0;
    CHECK(create(service, kEqualizer, 5, 0, &low_events, &low) == kSuccess);
    const Entry *entry = nullptr;
    // A probe of higher or equal priority would take control: success; a lower one would not: already exists.
    CHECK(service.probe(kEqualizer, kNull, 0, 5, &entry) == kSuccess && entry != nullptr);
    CHECK(service.probe(kEqualizer, kNull, 10, 5, &entry) == kSuccess);
    int high = 0;
    Events high_events;
    CHECK(create(service, kEqualizer, 5, 10, &high_events, &high) == kSuccess);
    low_events.seen.clear();
    high_events.seen.clear();
    CHECK(service.probe(kEqualizer, kNull, 0, 5, &entry) == kAlreadyExists);
    CHECK(service.probe(kEqualizer, kNull, 10, 5, &entry) == kSuccess);
    CHECK(service.probe(kEqualizer, kNull, 0, 6, &entry) == kSuccess);  // another session: nothing there
    CHECK(low_events.seen.empty() && high_events.seen.empty());           // no control moved, no callback
    CHECK(!service.get(low)->has_control && service.get(high)->has_control);
    CHECK(service.probe("58b4b260-8e06-11e0-aa8e-0002a5d5c51b", kNull, 0, 5, &entry) == kBadValue);
    service.release(high);
    CHECK(service.get(low)->has_control);
    // Only the two real instances ever existed: the next id is 3.
    int next = 0;
    CHECK(create(service, kLoudness, 5, 0, nullptr, &next) == kSuccess && next == 3);
}

}  // namespace

int main() {
    the_list_is_the_emulators();
    control_passes_between_instances_of_one_effect_on_a_session();
    enabling_needs_control_and_a_live_instance();
    the_equalizer_answers_with_the_emulators_values();
    the_equalizer_stores_levels_and_presets();
    the_other_effects_store_and_clamp();
    the_applied_settings_of_a_session_are_those_of_the_enabled_instances_in_control();
    the_reverbs_are_applied_as_the_environmental_values_of_the_preset_or_the_environment();
    a_probe_answers_as_a_create_would_without_making_an_instance();
    std::printf("audio_effect_service_test: %s\n", failures == 0 ? "all fine" : "failed");
    return failures == 0 ? 0 : 1;
}
