// AConfiguration of libandroid.so (ADR-0028, decision 9), written for Minato from the NDK's documentation
// and Android 15's answers (fixtures/suite/configuration-test.c). Each field is kept as wide as Android
// keeps it, several as bits of one byte, so a value beyond its range reads back cut to those bits; the
// language and the country are their first two characters. diff sets one ACONFIGURATION_* bit for each
// kind of field that differs. AConfiguration_fromAssetManager (asset_manager.cpp) gives the values that
// minato_configuration_from_session chose from the session (decision 9).
#include <android/configuration.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

struct AConfiguration {
    uint16_t mcc, mnc;
    char language[2], country[2];
    uint8_t orientation, touchscreen;
    uint16_t density;
    uint8_t keyboard, navigation;
    uint8_t input_flags;     // keys hidden in bits 0-1, navigation hidden in bits 2-3
    uint16_t sdk_version;
    uint8_t screen_layout;   // size in bits 0-3, long in bits 4-5, layout direction in bits 6-7
    uint8_t ui_mode;         // type in bits 0-3, night in bits 4-5
    uint16_t smallest_screen_width_dp, screen_width_dp, screen_height_dp;
    uint8_t screen_layout2;  // round in bits 0-1
    uint8_t grammatical_gender;
};

namespace {

// A field of `mask`'s bits in `byte`, shifted down.
int32_t bits(uint8_t byte, uint8_t mask, int shift) {
    return (byte & mask) >> shift;
}

void set_bits(uint8_t& byte, uint8_t mask, int shift, int32_t value) {
    byte = static_cast<uint8_t>((byte & ~mask) | ((value << shift) & mask));
}

constexpr uint8_t kKeysHidden = 0x03, kNavHidden = 0x0c;
constexpr uint8_t kScreenSize = 0x0f, kScreenLong = 0x30, kLayoutDirection = 0xc0;
constexpr uint8_t kUiModeType = 0x0f, kUiModeNight = 0x30;
constexpr uint8_t kScreenRound = 0x03, kGrammaticalGender = 0x03;

void copy_code(char out[2], const char* code) {
    out[0] = code != nullptr ? code[0] : 0;
    out[1] = code != nullptr && code[0] != 0 ? code[1] : 0;
}

}  // namespace

extern "C" {

AConfiguration* AConfiguration_new() {
    return static_cast<AConfiguration*>(calloc(1, sizeof(AConfiguration)));
}

void AConfiguration_delete(AConfiguration* config) {
    free(config);
}

void AConfiguration_copy(AConfiguration* dest, AConfiguration* src) {
    *dest = *src;
}

int32_t AConfiguration_getMcc(AConfiguration* config) { return config->mcc; }
void AConfiguration_setMcc(AConfiguration* config, int32_t mcc) { config->mcc = static_cast<uint16_t>(mcc); }
int32_t AConfiguration_getMnc(AConfiguration* config) { return config->mnc; }
void AConfiguration_setMnc(AConfiguration* config, int32_t mnc) { config->mnc = static_cast<uint16_t>(mnc); }

void AConfiguration_getLanguage(AConfiguration* config, char* outLanguage) {
    memcpy(outLanguage, config->language, 2);
}

void AConfiguration_setLanguage(AConfiguration* config, const char* language) {
    copy_code(config->language, language);
}

void AConfiguration_getCountry(AConfiguration* config, char* outCountry) {
    memcpy(outCountry, config->country, 2);
}

void AConfiguration_setCountry(AConfiguration* config, const char* country) {
    copy_code(config->country, country);
}

int32_t AConfiguration_getOrientation(AConfiguration* config) { return config->orientation; }
void AConfiguration_setOrientation(AConfiguration* config, int32_t value) {
    config->orientation = static_cast<uint8_t>(value);
}
int32_t AConfiguration_getTouchscreen(AConfiguration* config) { return config->touchscreen; }
void AConfiguration_setTouchscreen(AConfiguration* config, int32_t value) {
    config->touchscreen = static_cast<uint8_t>(value);
}
int32_t AConfiguration_getDensity(AConfiguration* config) { return config->density; }
void AConfiguration_setDensity(AConfiguration* config, int32_t value) { config->density = static_cast<uint16_t>(value); }
int32_t AConfiguration_getKeyboard(AConfiguration* config) { return config->keyboard; }
void AConfiguration_setKeyboard(AConfiguration* config, int32_t value) { config->keyboard = static_cast<uint8_t>(value); }
int32_t AConfiguration_getNavigation(AConfiguration* config) { return config->navigation; }
void AConfiguration_setNavigation(AConfiguration* config, int32_t value) {
    config->navigation = static_cast<uint8_t>(value);
}

int32_t AConfiguration_getKeysHidden(AConfiguration* config) { return bits(config->input_flags, kKeysHidden, 0); }
void AConfiguration_setKeysHidden(AConfiguration* config, int32_t value) {
    set_bits(config->input_flags, kKeysHidden, 0, value);
}
int32_t AConfiguration_getNavHidden(AConfiguration* config) { return bits(config->input_flags, kNavHidden, 2); }
void AConfiguration_setNavHidden(AConfiguration* config, int32_t value) {
    set_bits(config->input_flags, kNavHidden, 2, value);
}

int32_t AConfiguration_getSdkVersion(AConfiguration* config) { return config->sdk_version; }
void AConfiguration_setSdkVersion(AConfiguration* config, int32_t value) {
    config->sdk_version = static_cast<uint16_t>(value);
}

int32_t AConfiguration_getScreenSize(AConfiguration* config) { return bits(config->screen_layout, kScreenSize, 0); }
void AConfiguration_setScreenSize(AConfiguration* config, int32_t value) {
    set_bits(config->screen_layout, kScreenSize, 0, value);
}
int32_t AConfiguration_getScreenLong(AConfiguration* config) { return bits(config->screen_layout, kScreenLong, 4); }
void AConfiguration_setScreenLong(AConfiguration* config, int32_t value) {
    set_bits(config->screen_layout, kScreenLong, 4, value);
}
int32_t AConfiguration_getLayoutDirection(AConfiguration* config) {
    return bits(config->screen_layout, kLayoutDirection, 6);
}
void AConfiguration_setLayoutDirection(AConfiguration* config, int32_t value) {
    set_bits(config->screen_layout, kLayoutDirection, 6, value);
}
// The NDK exports no setter of its own for the round screen.
int32_t AConfiguration_getScreenRound(AConfiguration* config) { return bits(config->screen_layout2, kScreenRound, 0); }

int32_t AConfiguration_getUiModeType(AConfiguration* config) { return bits(config->ui_mode, kUiModeType, 0); }
void AConfiguration_setUiModeType(AConfiguration* config, int32_t value) {
    set_bits(config->ui_mode, kUiModeType, 0, value);
}
int32_t AConfiguration_getUiModeNight(AConfiguration* config) { return bits(config->ui_mode, kUiModeNight, 4); }
void AConfiguration_setUiModeNight(AConfiguration* config, int32_t value) {
    set_bits(config->ui_mode, kUiModeNight, 4, value);
}

int32_t AConfiguration_getScreenWidthDp(AConfiguration* config) { return config->screen_width_dp; }
void AConfiguration_setScreenWidthDp(AConfiguration* config, int32_t value) {
    config->screen_width_dp = static_cast<uint16_t>(value);
}
int32_t AConfiguration_getScreenHeightDp(AConfiguration* config) { return config->screen_height_dp; }
void AConfiguration_setScreenHeightDp(AConfiguration* config, int32_t value) {
    config->screen_height_dp = static_cast<uint16_t>(value);
}
int32_t AConfiguration_getSmallestScreenWidthDp(AConfiguration* config) { return config->smallest_screen_width_dp; }
void AConfiguration_setSmallestScreenWidthDp(AConfiguration* config, int32_t value) {
    config->smallest_screen_width_dp = static_cast<uint16_t>(value);
}

int32_t AConfiguration_getGrammaticalGender(AConfiguration* config) {
    return bits(config->grammatical_gender, kGrammaticalGender, 0);
}
void AConfiguration_setGrammaticalGender(AConfiguration* config, int32_t value) {
    set_bits(config->grammatical_gender, kGrammaticalGender, 0, value);
}

int32_t AConfiguration_diff(AConfiguration* config1, AConfiguration* config2) {
    const AConfiguration& a = *config1;
    const AConfiguration& b = *config2;
    int32_t diff = 0;
    if (a.mcc != b.mcc) diff |= ACONFIGURATION_MCC;
    if (a.mnc != b.mnc) diff |= ACONFIGURATION_MNC;
    if (memcmp(a.language, b.language, 2) != 0 || memcmp(a.country, b.country, 2) != 0) diff |= ACONFIGURATION_LOCALE;
    if (a.orientation != b.orientation) diff |= ACONFIGURATION_ORIENTATION;
    if (a.density != b.density) diff |= ACONFIGURATION_DENSITY;
    if (a.touchscreen != b.touchscreen) diff |= ACONFIGURATION_TOUCHSCREEN;
    if (((a.input_flags ^ b.input_flags) & (kKeysHidden | kNavHidden)) != 0) diff |= ACONFIGURATION_KEYBOARD_HIDDEN;
    if (a.keyboard != b.keyboard) diff |= ACONFIGURATION_KEYBOARD;
    if (a.navigation != b.navigation) diff |= ACONFIGURATION_NAVIGATION;
    if (a.screen_width_dp != b.screen_width_dp || a.screen_height_dp != b.screen_height_dp) {
        diff |= ACONFIGURATION_SCREEN_SIZE;
    }
    if (a.sdk_version != b.sdk_version) diff |= ACONFIGURATION_VERSION;
    if (((a.screen_layout ^ b.screen_layout) & kLayoutDirection) != 0) diff |= ACONFIGURATION_LAYOUTDIR;
    if (((a.screen_layout ^ b.screen_layout) & (kScreenSize | kScreenLong)) != 0) diff |= ACONFIGURATION_SCREEN_LAYOUT;
    if (((a.screen_layout2 ^ b.screen_layout2) & kScreenRound) != 0) diff |= ACONFIGURATION_SCREEN_ROUND;
    if (a.ui_mode != b.ui_mode) diff |= ACONFIGURATION_UI_MODE;
    if (a.smallest_screen_width_dp != b.smallest_screen_width_dp) diff |= ACONFIGURATION_SMALLEST_SCREEN_SIZE;
    if (a.grammatical_gender != b.grammatical_gender) diff |= ACONFIGURATION_GRAMMATICAL_GENDER;
    return diff;
}

}  // extern "C"

namespace {

// The languages whose script runs from right to left [inferred from ICU's likely scripts].
bool right_to_left(const char* language) {
    static const char kLanguages[][3] = {"ar", "dv", "fa", "he", "iw", "ji", "ks", "ps", "sd", "ug", "ur", "yi"};
    for (const char* candidate : kLanguages) {
        if (language[0] == candidate[0] && language[0] != 0 && language[1] == candidate[1]) return true;
    }
    return false;
}

}  // namespace

// Decision 9's values. The size and length of the screen follow Android's Configuration.reduceScreenLayout
// [inferred]: small below 470 dp on the long side, extra large from 960 × 720 dp, large from 640 × 480 dp,
// else normal; long when the long side's three fifths reach the short side less 1.
void minato_configuration_from_session(AConfiguration* config, const struct apkrt_configuration* session) {
    *config = AConfiguration{};
    const double scale = session->scale > 0 && session->scale < 64 ? session->scale : 1;
    int32_t density = static_cast<int32_t>(scale * 160 + 0.5);
    if (density < 1) density = 1;
    const int32_t width_dp = static_cast<int32_t>(static_cast<int64_t>(session->width) * 160 / density);
    const int32_t height_dp = static_cast<int32_t>(static_cast<int64_t>(session->height) * 160 / density);
    const int32_t long_dp = width_dp > height_dp ? width_dp : height_dp;
    const int32_t short_dp = width_dp > height_dp ? height_dp : width_dp;
    int32_t size = ACONFIGURATION_SCREENSIZE_NORMAL;
    if (long_dp < 470) {
        size = ACONFIGURATION_SCREENSIZE_SMALL;
    } else if (long_dp >= 960 && short_dp >= 720) {
        size = ACONFIGURATION_SCREENSIZE_XLARGE;
    } else if (long_dp >= 640 && short_dp >= 480) {
        size = ACONFIGURATION_SCREENSIZE_LARGE;
    }
    const bool is_long = long_dp >= 470 && long_dp * 3 / 5 >= short_dp - 1;
    AConfiguration_setDensity(config, density);
    AConfiguration_setOrientation(config, session->orientation);
    AConfiguration_setTouchscreen(config, ACONFIGURATION_TOUCHSCREEN_FINGER);
    AConfiguration_setKeyboard(config, ACONFIGURATION_KEYBOARD_NOKEYS);
    AConfiguration_setKeysHidden(config, ACONFIGURATION_KEYSHIDDEN_YES);
    AConfiguration_setNavigation(config, ACONFIGURATION_NAVIGATION_NONAV);
    AConfiguration_setScreenSize(config, size);
    AConfiguration_setScreenLong(config, is_long ? ACONFIGURATION_SCREENLONG_YES : ACONFIGURATION_SCREENLONG_NO);
    set_bits(config->screen_layout2, kScreenRound, 0, ACONFIGURATION_SCREENROUND_NO);
    AConfiguration_setUiModeType(config, ACONFIGURATION_UI_MODE_TYPE_NORMAL);
    AConfiguration_setUiModeNight(config, session->night);
    AConfiguration_setScreenWidthDp(config, width_dp);
    AConfiguration_setScreenHeightDp(config, height_dp);
    AConfiguration_setSmallestScreenWidthDp(config, short_dp);
    AConfiguration_setLanguage(config, session->language);
    AConfiguration_setCountry(config, session->country);
    AConfiguration_setLayoutDirection(config, right_to_left(session->language) ? ACONFIGURATION_LAYOUTDIR_RTL
                                                                               : ACONFIGURATION_LAYOUTDIR_LTR);
    AConfiguration_setSdkVersion(config, 35);
}
