// Ours (the audio effects plan): the local effect service behind android.media.audiofx.AudioEffect's natives. Android's
// audio server lists the effects of its libeffects and hands out instances with parameters in the byte protocol of
// system/audio_effects/effect_*.h; here the same list (Android 15's emulator, measured by audiofx-test), instances kept
// per session with their enabled state and parameters, control passing between instances of one effect on a session,
// and the parameters of the effects players create (Equalizer, Dynamic Bass Boost, Virtualizer, Loudness Enhancer,
// Preset Reverb, Environmental Reverb) stored and read back with the ranges, defaults and error codes of Android's.
// The sound does not change: no sample passes through. Plain C++ without JNI, so that
// framework/tests/native/audio_effect_service_test.cpp runs it on the host.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace minato::audiofx {

// AudioEffect.java's status codes.
constexpr int kSuccess = 0, kError = -1, kAlreadyExists = -2, kNoInit = -3, kBadValue = -4, kInvalidOperation = -5;

struct Descriptor {
    const char *type;
    const char *uuid;
    const char *mode;  // "Insert", "Auxiliary" or "Pre Processing"
    const char *name;
    const char *implementor;
};

enum class Kind { none, equalizer, bass_boost, virtualizer, loudness, preset_reverb, env_reverb, other };

struct Entry {
    Descriptor descriptor;
    Kind kind;
};

/// The effects Android 15's emulator lists, in its order.
inline const std::vector<Entry> &entries() {
    static const std::vector<Entry> list = {
        {{"1411e6d6-aecd-4021-a1cf-a6aceb0d71e5", "97c4acd1-8b82-4f2f-832e-c2fe5d7a9931", "Insert", "Haptic Generator",
          "The Android Open Source Project"}, Kind::other},
        {{"7261676f-6d75-7369-6364-28e2fd3ac39e", "e0e6539b-1781-7261-676f-6d7573696340", "Insert",
          "Dynamics Processing", "The Android Open Source Project"}, Kind::other},
        {{"fe3199be-aed0-413f-87bb-11260eb63cf1", "fa415329-2034-4bea-b5dc-5b381c8d1e2c", "Insert",
          "Loudness Enhancer", "The Android Open Source Project"}, Kind::loudness},
        {{"381e49cc-a858-4aa2-87f6-e8388e7601b2", "93f04452-e4fe-41cc-91f9-e475b6d1d69f", "Insert",
          "Multichannel Downmix To Stereo", "The Android Open Source Project"}, Kind::other},
        {{"e46b26a0-dddd-11db-8afd-0002a5d5c51b", "d069d9e0-8329-11df-9168-0002a5d5c51b", "Insert", "Visualizer",
          "The Android Open Source Project"}, Kind::other},
        {{"47382d60-ddd8-11db-bf3a-0002a5d5c51b", "172cdf00-a3bc-11df-a72f-0002a5d5c51b", "Insert",
          "Insert Preset Reverb", "NXP Software Ltd."}, Kind::preset_reverb},
        {{"47382d60-ddd8-11db-bf3a-0002a5d5c51b", "f29a1400-a3bb-11df-8ddc-0002a5d5c51b", "Auxiliary",
          "Auxiliary Preset Reverb", "NXP Software Ltd."}, Kind::preset_reverb},
        {{"c2e5d5f0-94bd-4763-9cac-4e234d06839e", "c7a511a0-a3bb-11df-860e-0002a5d5c51b", "Insert",
          "Insert Environmental Reverb", "NXP Software Ltd."}, Kind::env_reverb},
        {{"c2e5d5f0-94bd-4763-9cac-4e234d06839e", "4a387fc0-8ab3-11df-8bad-0002a5d5c51b", "Auxiliary",
          "Auxiliary Environmental Reverb", "NXP Software Ltd."}, Kind::env_reverb},
        {{"09e8ede0-ddde-11db-b4f6-0002a5d5c51b", "119341a0-8469-11df-81f9-0002a5d5c51b", "Insert", "Volume",
          "NXP Software Ltd."}, Kind::other},
        {{"0bed4300-ddd6-11db-8f34-0002a5d5c51b", "ce772f20-847d-11df-bb17-0002a5d5c51b", "Insert", "Equalizer",
          "NXP Software Ltd."}, Kind::equalizer},
        {{"37cc2c00-dddd-11db-8577-0002a5d5c51b", "1d4033c0-8557-11df-9f2d-0002a5d5c51b", "Insert", "Virtualizer",
          "NXP Software Ltd."}, Kind::virtualizer},
        {{"0634f220-ddd4-11db-a0fc-0002a5d5c51b", "8631f300-72e2-11df-b57e-0002a5d5c51b", "Insert",
          "Dynamic Bass Boost", "NXP Software Ltd."}, Kind::bass_boost},
    };
    return list;
}

constexpr int kBands = 5, kPresets = 10;
constexpr int16_t kLevelMin = -1500, kLevelMax = 1500;
constexpr int32_t kCenter[kBands] = {60000, 230000, 910000, 3600000, 14000000};
constexpr int32_t kLow[kBands] = {30000, 120001, 460001, 1800001, 7000001};
constexpr int32_t kHigh[kBands] = {120000, 460000, 1800000, 7000000, 20000000};
inline const char *preset_name(int preset) {
    static const char *const names[kPresets] = {"Normal", "Classical", "Dance", "Flat", "Folk",
                                                "Heavy Metal", "Hip Hop", "Jazz", "Pop", "Rock"};
    return names[preset];
}
// The bundle's presets, in dB per band (the levels in mB are 100 times these).
constexpr int16_t kPresetLevels[kPresets][kBands] = {
    {3, 0, 0, 0, 3},  {5, 3, -2, 4, 4}, {6, 0, 2, 4, 1},  {0, 0, 0, 0, 0},  {3, 0, 0, 2, -1},
    {4, 1, 9, 3, 0},  {5, 3, 0, 1, 3},  {4, 2, -2, 2, 5}, {-1, 2, 5, 1, -2}, {5, 3, -1, 3, 5}};

/// The preset reverb's presets as the effect bundle has them (EffectReverb.cpp, sReverbPresets, android-15.0.0_r1), in
/// the order of the environmental reverb's values: room level, room HF level, decay time, decay HF ratio, reflections
/// level, reflections delay, reverb level, reverb delay, diffusion, density. Preset 0 is none: no sound.
constexpr int kReverbPresets = 7;
constexpr int32_t kReverbPresetValues[kReverbPresets][10] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {-400, -600, 1100, 830, -400, 5, 500, 10, 1000, 1000},
    {-400, -600, 1300, 830, -1000, 20, -200, 20, 1000, 1000},
    {-400, -600, 1500, 830, -1600, 5, -1000, 40, 1000, 1000},
    {-400, -600, 1800, 700, -1300, 15, -800, 30, 1000, 1000},
    {-400, -600, 1800, 700, -2000, 30, -1400, 60, 1000, 1000},
    {-400, -200, 1300, 900, 0, 2, 0, 10, 1000, 750}};

/// The environmental reverb's parameters in effect_environmentalreverb.h's order; shorts and ints as the protocol
/// has them.
struct EnvReverb {
    int16_t room_level = -6000, room_hf_level = 0;
    int32_t decay_time = 1490;
    int16_t decay_hf_ratio = 420, reflections_level = 0;
    int32_t reflections_delay = 0;
    int16_t reverb_level = -6000;
    int32_t reverb_delay = 0;
    int16_t diffusion = 1000, density = 1000;
};

struct Instance {
    int id = 0;
    Kind kind = Kind::none;
    const Entry *entry = nullptr;
    int session = 0;
    int priority = 0;
    uint64_t serial = 0;  // creation order
    bool enabled = false;
    bool has_control = true;
    // the parameters
    int16_t levels[kBands] = {300, 0, 0, 0, 300};
    int16_t preset = 0;
    int16_t strength = 0;
    int32_t target_gain = 0;
    int16_t reverb_preset = 0;
    EnvReverb env;
};

inline int32_t read_int(const uint8_t *bytes, size_t at = 0) {
    int32_t value;
    std::memcpy(&value, bytes + at, sizeof value);
    return value;
}
inline void write_short(std::vector<uint8_t> &out, int16_t value) {
    out.insert(out.end(), reinterpret_cast<uint8_t *>(&value), reinterpret_cast<uint8_t *>(&value) + 2);
}
inline void write_int(std::vector<uint8_t> &out, int32_t value) {
    out.insert(out.end(), reinterpret_cast<uint8_t *>(&value), reinterpret_cast<uint8_t *>(&value) + 4);
}

/// What a session's instances ask the runtime's mix to play: of each of the four sounding effects the instance in
/// control that is enabled, with its parameters (the runtime's audio_effect takes these).
struct Applied {
    bool equalizer = false;
    int32_t bands[kBands] = {0, 0, 0, 0, 0};
    bool bass_boost = false;
    int32_t bass_strength = 0;
    bool virtualizer = false;
    int32_t virtualizer_strength = 0;
    bool loudness = false;
    int32_t loudness_millibel = 0;
    /// One reverb: the environmental reverb's room level, room HF level, decay time, decay HF ratio, reverb level,
    /// diffusion and density (the reflections are not played, as in Android's bundle).
    bool reverb = false;
    int32_t reverb_params[7] = {0, 0, 0, 0, 0, 0, 0};
};

/// The instances, with the control passing of AudioEffect: of the instances of one implementation on a session, the
/// one with the highest priority has control, the newest among equals; the others do not, and get it back in that order
/// when it is released. `on_control` tells an instance that gained or lost it.
class Service {
public:
    using ControlListener = void (*)(void *context, int id, bool has_control);

    static const Entry *find(const std::string &type, const std::string &uuid) {
        for (const Entry &entry : entries()) {
            static const std::string any = "ec7178ec-e5e1-4432-a3f4-4657e6795210";  // EFFECT_TYPE_NULL: no preference
            if (!uuid.empty() && uuid != any && uuid != entry.descriptor.uuid) continue;
            if (!type.empty() && type != any && type != entry.descriptor.type) continue;
            return &entry;
        }
        return nullptr;
    }

    /// Creates an instance: its id, kSuccess when it has control or kAlreadyExists when it has not, kBadValue for an
    /// effect the list lacks.
    int create(const std::string &type, const std::string &uuid, int priority, int session, int *id,
               const Entry **entry, ControlListener listener, void *context) {
        const Entry *found = find(type, uuid);
        if (found == nullptr) return kBadValue;
        auto instance = std::make_unique<Instance>();
        instance->id = ++next_id_;
        instance->kind = found->kind;
        instance->entry = found;
        instance->session = session;
        instance->priority = priority;
        instance->serial = ++serial_;
        *id = instance->id;
        *entry = found;
        listeners_[instance->id] = {listener, context};
        Instance *raw = instance.get();
        instances_[raw->id] = std::move(instance);
        rearrange(raw->session, raw->entry->descriptor.uuid, raw->id);
        return raw->has_control ? kSuccess : kAlreadyExists;
    }

    /// What create would answer, without making an instance: kSuccess when an instance of that priority would have
    /// control (none of that session and effect has a higher one), kAlreadyExists when it would not, kBadValue for an
    /// effect the list lacks. No instance moves and no listener is told.
    int probe(const std::string &type, const std::string &uuid, int priority, int session, const Entry **entry) const {
        const Entry *found = find(type, uuid);
        if (found == nullptr) return kBadValue;
        *entry = found;
        for (const auto &[id, instance] : instances_) {
            if (instance->session == session && uuid_of(*instance) == found->descriptor.uuid &&
                instance->priority > priority) {
                return kAlreadyExists;
            }
        }
        return kSuccess;
    }

    void release(int id) {
        auto found = instances_.find(id);
        if (found == instances_.end()) return;
        const int session = found->second->session;
        const std::string uuid = found->second->entry->descriptor.uuid;
        instances_.erase(found);
        listeners_.erase(id);
        rearrange(session, uuid, 0);
    }

    Instance *get(int id) {
        auto found = instances_.find(id);
        return found == instances_.end() ? nullptr : found->second.get();
    }

    int set_enabled(int id, bool enabled) {
        Instance *instance = get(id);
        if (instance == nullptr) return kNoInit;
        if (!instance->has_control) return kInvalidOperation;
        instance->enabled = enabled;
        return kSuccess;
    }

    /// The effects of `session` that sound now.
    Applied applied(int session) const {
        Applied result;
        bool environment = false;  // an environmental reverb of the session is on
        for (const auto &[id, instance] : instances_) {
            if (instance->session == session && instance->has_control && instance->enabled &&
                instance->kind == Kind::env_reverb) {
                environment = true;
            }
        }
        for (const auto &[id, instance] : instances_) {
            if (instance->session != session || !instance->has_control || !instance->enabled) continue;
            switch (instance->kind) {
                case Kind::equalizer:
                    result.equalizer = true;
                    for (int band = 0; band < kBands; ++band) result.bands[band] = instance->levels[band];
                    break;
                case Kind::bass_boost:
                    result.bass_boost = true;
                    result.bass_strength = instance->strength;
                    break;
                case Kind::virtualizer:
                    result.virtualizer = true;
                    result.virtualizer_strength = instance->strength;
                    break;
                case Kind::loudness:
                    result.loudness = true;
                    result.loudness_millibel = instance->target_gain;
                    break;
                case Kind::preset_reverb:
                    // the preset's row, unless an environmental reverb is on (it plays instead, whatever the order)
                    if (instance->reverb_preset > 0 && instance->reverb_preset < kReverbPresets && !environment) {
                        const int32_t *row = kReverbPresetValues[instance->reverb_preset];
                        const int32_t values[7] = {row[0], row[1], row[2], row[3], row[6], row[8], row[9]};
                        std::memcpy(result.reverb_params, values, sizeof values);
                        result.reverb = true;
                    }
                    break;
                case Kind::env_reverb: {
                    const EnvReverb &e = instance->env;
                    const int32_t values[7] = {e.room_level, e.room_hf_level, e.decay_time, e.decay_hf_ratio,
                                               e.reverb_level, e.diffusion, e.density};
                    std::memcpy(result.reverb_params, values, sizeof values);
                    result.reverb = true;
                    break;
                }
                default: break;
            }
        }
        return result;
    }

    /// The parameter `param` (psize bytes) read into `value` (vsize bytes): the bytes written, or a negative status.
    int get_parameter(int id, const uint8_t *param, int psize, uint8_t *value, int vsize) {
        Instance *instance = get(id);
        if (instance == nullptr) return kNoInit;
        std::vector<uint8_t> out;
        const int status = read_parameter(*instance, param, psize, out);
        if (status != kSuccess) return status;
        if (static_cast<int>(out.size()) > vsize) return kBadValue;
        std::memcpy(value, out.data(), out.size());
        return static_cast<int>(out.size());
    }

    int set_parameter(int id, const uint8_t *param, int psize, const uint8_t *value, int vsize) {
        Instance *instance = get(id);
        if (instance == nullptr) return kNoInit;
        if (!instance->has_control) return kInvalidOperation;
        return write_parameter(*instance, param, psize, value, vsize);
    }

private:
    static std::string uuid_of(const Instance &instance) { return instance.entry->descriptor.uuid; }

    struct Listener {
        ControlListener function = nullptr;
        void *context = nullptr;
    };

    /// Gives control to the right instance of (session, implementation), telling the ones that change; `created` is the
    /// instance that just came, whose own gain is not announced (its creation tells).
    void rearrange(int session, const std::string &uuid, int created) {
        Instance *best = nullptr;
        for (auto &[id, instance] : instances_) {
            if (instance->session != session || uuid != instance->entry->descriptor.uuid) continue;
            if (best == nullptr || instance->priority > best->priority ||
                (instance->priority == best->priority && instance->serial > best->serial)) {
                best = instance.get();
            }
        }
        for (auto &[id, instance] : instances_) {
            if (instance->session != session || uuid != instance->entry->descriptor.uuid) continue;
            const bool wants = instance.get() == best;
            if (instance->has_control == wants) continue;
            instance->has_control = wants;
            const Listener &listener = listeners_[id];
            if (id != created && listener.function != nullptr) listener.function(listener.context, id, wants);
        }
    }

    static int read_parameter(Instance &fx, const uint8_t *param, int psize, std::vector<uint8_t> &out) {
        if (psize < 4) return kBadValue;
        const int32_t id = read_int(param);
        const int32_t arg = psize >= 8 ? read_int(param, 4) : 0;
        switch (fx.kind) {
            case Kind::equalizer:
                switch (id) {
                    case 0: write_short(out, kBands); return kSuccess;
                    case 1: write_short(out, kLevelMin); write_short(out, kLevelMax); return kSuccess;
                    case 2:
                        if (psize < 8 || arg < 0 || arg >= kBands) return kBadValue;
                        write_short(out, fx.levels[arg]);
                        return kSuccess;
                    case 3:
                        if (psize < 8 || arg < 0 || arg >= kBands) return kBadValue;
                        write_int(out, kCenter[arg]);
                        return kSuccess;
                    case 4:
                        if (psize < 8 || arg < 0 || arg >= kBands) return kBadValue;
                        write_int(out, kLow[arg]);
                        write_int(out, kHigh[arg]);
                        return kSuccess;
                    case 5: {
                        if (psize < 8) return kBadValue;
                        int16_t band = -22;  // as the bundle answers a frequency outside every band
                        for (int index = 0; index < kBands; ++index) {
                            if (arg >= kLow[index] && arg <= kHigh[index]) band = static_cast<int16_t>(index);
                        }
                        write_short(out, band);
                        return kSuccess;
                    }
                    case 6: write_short(out, fx.preset); return kSuccess;
                    case 7: write_short(out, kPresets); return kSuccess;
                    case 8: {
                        if (psize < 8 || arg < 0 || arg >= kPresets) return kBadValue;
                        const char *name = preset_name(arg);
                        out.insert(out.end(), name, name + std::strlen(name) + 1);
                        return kSuccess;
                    }
                    case 9:
                        write_short(out, fx.preset);
                        write_short(out, kBands);
                        for (int16_t level : fx.levels) write_short(out, level);
                        return kSuccess;
                    default: return kBadValue;
                }
            case Kind::bass_boost:
            case Kind::virtualizer:
                switch (id) {
                    case 0: write_int(out, 1); return kSuccess;
                    case 1: write_short(out, fx.strength); return kSuccess;
                    case 4:
                        if (fx.kind == Kind::virtualizer) { write_int(out, 0); return kSuccess; }
                        return kBadValue;
                    default: return kBadValue;
                }
            case Kind::loudness:
                if (id != 0) return kBadValue;
                write_int(out, fx.target_gain);
                return kSuccess;
            case Kind::preset_reverb:
                if (id != 0) return kBadValue;
                write_short(out, fx.reverb_preset);
                return kSuccess;
            case Kind::env_reverb: {
                const EnvReverb &e = fx.env;
                switch (id) {
                    case 0: write_short(out, e.room_level); return kSuccess;
                    case 1: write_short(out, e.room_hf_level); return kSuccess;
                    case 2: write_int(out, e.decay_time); return kSuccess;
                    case 3: write_short(out, e.decay_hf_ratio); return kSuccess;
                    case 4: write_short(out, e.reflections_level); return kSuccess;
                    case 5: write_int(out, e.reflections_delay); return kSuccess;
                    case 6: write_short(out, e.reverb_level); return kSuccess;
                    case 7: write_int(out, e.reverb_delay); return kSuccess;
                    case 8: write_short(out, e.diffusion); return kSuccess;
                    case 9: write_short(out, e.density); return kSuccess;
                    default: return kBadValue;
                }
            }
            default:
                return kBadValue;
        }
    }

    static int16_t short_at(const uint8_t *value, int vsize, int at = 0, bool *ok = nullptr) {
        int16_t result = 0;
        if (vsize >= at + 2) std::memcpy(&result, value + at, 2);
        if (ok != nullptr) *ok = vsize >= at + 2;
        return result;
    }

    static int write_parameter(Instance &fx, const uint8_t *param, int psize, const uint8_t *value, int vsize) {
        if (psize < 4) return kBadValue;
        const int32_t id = read_int(param);
        const int32_t arg = psize >= 8 ? read_int(param, 4) : 0;
        bool ok = false;
        switch (fx.kind) {
            case Kind::equalizer:
                switch (id) {
                    case 2: {
                        const int16_t level = short_at(value, vsize, 0, &ok);
                        // The level itself is not checked.
                        if (!ok || psize < 8 || arg < 0 || arg >= kBands) return kBadValue;
                        fx.levels[arg] = level;
                        fx.preset = -1;  // a band changed by hand: no preset
                        return kSuccess;
                    }
                    case 6: {
                        const int16_t preset = short_at(value, vsize, 0, &ok);
                        if (!ok || preset < 0 || preset >= kPresets) return kBadValue;
                        fx.preset = preset;
                        for (int band = 0; band < kBands; ++band) {
                            fx.levels[band] = static_cast<int16_t>(kPresetLevels[preset][band] * 100);
                        }
                        return kSuccess;
                    }
                    case 9: {
                        if (vsize < 4 + 2 * kBands) return kBadValue;
                        const int16_t preset = short_at(value, vsize, 0);
                        fx.preset = preset;
                        for (int band = 0; band < kBands; ++band) {
                            fx.levels[band] = short_at(value, vsize, 4 + 2 * band);
                        }
                        return kSuccess;
                    }
                    default: return kBadValue;
                }
            case Kind::bass_boost:
            case Kind::virtualizer:
                if (id == 1) {
                    const int16_t strength = short_at(value, vsize, 0, &ok);
                    if (!ok) return kBadValue;
                    fx.strength = strength;  // kept as given, as Android's engine keeps it
                    return kSuccess;
                }
                return kBadValue;
            case Kind::loudness:
                if (id != 0 || vsize < 4) return kBadValue;
                fx.target_gain = read_int(value);
                return kSuccess;
            case Kind::preset_reverb: {
                const int16_t preset = short_at(value, vsize, 0, &ok);
                if (id != 0 || !ok || preset < 0 || preset > 6) return kBadValue;
                fx.reverb_preset = preset;
                return kSuccess;
            }
            case Kind::env_reverb: {
                EnvReverb &e = fx.env;
                const int16_t s = short_at(value, vsize, 0, &ok);
                switch (id) {
                    case 0: if (!ok) return kBadValue; e.room_level = s; return kSuccess;
                    case 1: if (!ok) return kBadValue; e.room_hf_level = s; return kSuccess;
                    case 2: if (vsize < 4) return kBadValue; e.decay_time = read_int(value); return kSuccess;
                    case 3: if (!ok) return kBadValue; e.decay_hf_ratio = s; return kSuccess;
                    case 4: if (!ok) return kBadValue; e.reflections_level = s; return kSuccess;
                    case 5: if (vsize < 4) return kBadValue; e.reflections_delay = read_int(value); return kSuccess;
                    case 6: if (!ok) return kBadValue; e.reverb_level = s; return kSuccess;
                    case 7: if (vsize < 4) return kBadValue; e.reverb_delay = read_int(value); return kSuccess;
                    case 8: if (!ok) return kBadValue; e.diffusion = s; return kSuccess;
                    case 9: if (!ok) return kBadValue; e.density = s; return kSuccess;
                    default: return kBadValue;
                }
            }
            default:
                return kBadValue;
        }
    }

    std::map<int, std::unique_ptr<Instance>> instances_;
    std::map<int, Listener> listeners_;
    int next_id_ = 0;
    uint64_t serial_ = 0;
};

}  // namespace minato::audiofx
