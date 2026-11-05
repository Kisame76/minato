// The state of android.media.audiofx.Visualizer (framework/native/android_runtime/visualizer_state.h) on the host, with
// the answers Android 15 gave in visualizer-test (fixtures/expected/visualizer-test-granted.logcat).
#include "../../native/android_runtime/visualizer_state.h"

#include <cstdio>

using namespace minato::visualizer;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

void the_capture_size_takes_powers_of_two_in_the_range_while_disabled() {
    State v;
    CHECK(v.capture_size == 1024);
    CHECK(v.set_capture_size(256) == kSuccess && v.capture_size == 256);
    CHECK(v.set_capture_size(3) == kBadValue && v.capture_size == 256);
    CHECK(v.set_capture_size(64) == kBadValue);
    CHECK(v.set_capture_size(2048) == kBadValue);
    CHECK(v.set_capture_size(1000) == kBadValue);
    v.set_enabled(true);
    CHECK(v.set_capture_size(512) == kInvalidOperation && v.capture_size == 256);
}

void the_modes_take_two_values() {
    State v;
    CHECK(v.set_scaling_mode(1) == kSuccess && v.scaling_mode == 1);
    CHECK(v.set_scaling_mode(7) == kBadValue && v.scaling_mode == 1);
    CHECK(v.set_measurement_mode(1) == kSuccess && v.measurement_mode == 1);
    CHECK(v.set_measurement_mode(-1) == kBadValue);
}

void the_data_needs_the_enabled_state_and_a_big_enough_array() {
    State v;
    std::vector<uint8_t> out;
    CHECK(v.get_waveform(out, 1024) == kInvalidOperation);
    v.set_capture_size(256);
    v.set_enabled(true);
    CHECK(v.get_waveform(out, 255) == kBadValue);
    CHECK(v.get_waveform(out, 256) == kSuccess && out.size() == 256 && out[0] == 128 && out[255] == 128);
    CHECK(v.get_fft(out, 256) == kSuccess && out.size() == 256 && out[10] == 0);
    int peak = 0, rms = 0;
    CHECK(v.get_peak_rms(&peak, &rms) == kInvalidOperation);
    v.set_enabled(false);
    v.set_measurement_mode(1);
    CHECK(v.get_peak_rms(&peak, &rms) == kSuccess && peak == -9600 && rms == -9600);
}

void the_periodic_capture_is_set_while_disabled() {
    State v;
    CHECK(!v.periodic());
    CHECK(v.set_periodic_capture(10000, true, true) == kSuccess && v.periodic());
    CHECK(v.set_periodic_capture(20001, true, true) == kBadValue);
    v.set_enabled(true);
    CHECK(v.set_periodic_capture(10000, true, false) == kInvalidOperation);
    v.set_enabled(false);
    CHECK(v.set_periodic_capture(10000, false, false) == kSuccess && !v.periodic());
    CHECK(v.set_periodic_capture(0, true, true) == kSuccess && !v.periodic());
}

}  // namespace

int main() {
    the_capture_size_takes_powers_of_two_in_the_range_while_disabled();
    the_modes_take_two_values();
    the_data_needs_the_enabled_state_and_a_big_enough_array();
    the_periodic_capture_is_set_while_disabled();
    std::printf("visualizer_state_test: %s\n", failures == 0 ? "all fine" : "failed");
    return failures == 0 ? 0 : 1;
}
