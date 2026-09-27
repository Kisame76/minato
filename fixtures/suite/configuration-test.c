// configuration-test: AConfiguration as Android 15's libandroid.so gives it (ADR-0028, decision 9): the
// values of AConfiguration_new, every setter read back by its getter, with a value in range and one
// beyond it (which shows how wide each field is kept), what copy copies, and the bits of diff for each
// field. Expected output: fixtures/expected/configuration-test.out
#include <android/configuration.h>
#include <stdio.h>
#include <string.h>

// The getters and setters of one field.
struct field {
    const char *name;
    int32_t (*get)(AConfiguration *);
    void (*set)(AConfiguration *, int32_t);
    int32_t in_range, beyond;
};

#define FIELD(name, in_range, beyond) \
    { #name, AConfiguration_get##name, AConfiguration_set##name, in_range, beyond }

static const struct field fields[] = {
    FIELD(Mcc, 310, 0x12345),
    FIELD(Mnc, ACONFIGURATION_MNC_ZERO, -2),
    FIELD(Orientation, ACONFIGURATION_ORIENTATION_LAND, 0x1ff),
    FIELD(Touchscreen, ACONFIGURATION_TOUCHSCREEN_FINGER, 300),
    FIELD(Density, ACONFIGURATION_DENSITY_XXHIGH, 0x10001),
    FIELD(Keyboard, ACONFIGURATION_KEYBOARD_QWERTY, 258),
    FIELD(Navigation, ACONFIGURATION_NAVIGATION_WHEEL, 260),
    FIELD(KeysHidden, ACONFIGURATION_KEYSHIDDEN_SOFT, 7),
    FIELD(NavHidden, ACONFIGURATION_NAVHIDDEN_YES, 7),
    FIELD(SdkVersion, 35, 0x10023),
    FIELD(ScreenSize, ACONFIGURATION_SCREENSIZE_XLARGE, 0x1f),
    FIELD(ScreenLong, ACONFIGURATION_SCREENLONG_YES, 7),
    // The NDK exports no AConfiguration_setScreenRound, though its header declares one.
    {"ScreenRound", AConfiguration_getScreenRound, NULL, 0, 0},
    FIELD(UiModeType, ACONFIGURATION_UI_MODE_TYPE_VR_HEADSET, 0x1f),
    FIELD(UiModeNight, ACONFIGURATION_UI_MODE_NIGHT_YES, 7),
    FIELD(ScreenWidthDp, 411, 0x10005),
    FIELD(ScreenHeightDp, 891, 70000),
    FIELD(SmallestScreenWidthDp, 411, 65536),
    FIELD(LayoutDirection, ACONFIGURATION_LAYOUTDIR_RTL, 7),
    FIELD(GrammaticalGender, ACONFIGURATION_GRAMMATICAL_GENDER_MASCULINE, 7),
};
#define FIELD_COUNT (sizeof fields / sizeof fields[0])

// Four bytes preset to '?', so that the line shows how many the getter writes.
static void print_code(const char *what, void (*get)(AConfiguration *, char *), AConfiguration *config) {
    char code[4] = {'?', '?', '?', '?'};
    get(config, code);
    printf("%s %02x %02x %02x %02x", what, (unsigned char)code[0], (unsigned char)code[1], (unsigned char)code[2],
           (unsigned char)code[3]);
}

static void print_all(const char *title, AConfiguration *config) {
    printf("%s:", title);
    for (size_t i = 0; i < FIELD_COUNT; ++i) printf(" %s %d", fields[i].name, fields[i].get(config));
    printf("\n ");
    print_code("language", AConfiguration_getLanguage, config);
    print_code(", country", AConfiguration_getCountry, config);
    printf("\n");
}

int main(void) {
    AConfiguration *config = AConfiguration_new();
    print_all("new", config);

    // Each setter read back by its getter, then the rest unchanged.
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        if (fields[i].set == NULL) continue;
        fields[i].set(config, fields[i].in_range);
        const int32_t in_range = fields[i].get(config);
        fields[i].set(config, fields[i].beyond);
        printf("set%s(%d): %d, set%s(%d): %d\n", fields[i].name, fields[i].in_range, in_range, fields[i].name,
               fields[i].beyond, fields[i].get(config));
    }
    print_all("after every setter", config);

    // An empty code as four zero bytes: the setters read the third byte to tell two letters from three.
    static const char none[4];
    const char *languages[] = {"de", "fil", "en-US", none};
    for (size_t i = 0; i < sizeof languages / sizeof languages[0]; ++i) {
        AConfiguration_setLanguage(config, languages[i]);
        printf("setLanguage(\"%s\"):", languages[i]);
        print_code("", AConfiguration_getLanguage, config);
        printf("\n");
    }
    const char *countries[] = {"AT", "419", "US", none};
    for (size_t i = 0; i < sizeof countries / sizeof countries[0]; ++i) {
        AConfiguration_setCountry(config, countries[i]);
        printf("setCountry(\"%s\"):", countries[i]);
        print_code("", AConfiguration_getCountry, config);
        printf("\n");
    }

    // copy copies every field.
    AConfiguration_setLanguage(config, "de");
    AConfiguration_setCountry(config, "AT");
    AConfiguration *copy = AConfiguration_new();
    AConfiguration_copy(copy, config);
    print_all("copy", copy);
    printf("diff of the copy: 0x%x\n", AConfiguration_diff(config, copy));

    // The bits of diff: a new configuration against one with a single field set.
    AConfiguration *base = AConfiguration_new();
    AConfiguration *other = AConfiguration_new();
    printf("diff of two new ones: 0x%x\n", AConfiguration_diff(base, other));
    AConfiguration_delete(other);
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        if (fields[i].set == NULL) continue;
        AConfiguration *changed = AConfiguration_new();
        fields[i].set(changed, fields[i].in_range);
        printf("diff for %s: 0x%x\n", fields[i].name, AConfiguration_diff(base, changed));
        AConfiguration_delete(changed);
    }
    AConfiguration *changed = AConfiguration_new();
    AConfiguration_setLanguage(changed, "de");
    printf("diff for Language: 0x%x\n", AConfiguration_diff(base, changed));
    AConfiguration_delete(changed);
    changed = AConfiguration_new();
    AConfiguration_setCountry(changed, "AT");
    printf("diff for Country: 0x%x\n", AConfiguration_diff(base, changed));
    printf("diff the other way round: 0x%x\n", AConfiguration_diff(changed, base));
    AConfiguration_setDensity(changed, ACONFIGURATION_DENSITY_HIGH);
    AConfiguration_setUiModeNight(changed, ACONFIGURATION_UI_MODE_NIGHT_NO);
    printf("diff for Country, Density and UiModeNight: 0x%x\n", AConfiguration_diff(base, changed));
    AConfiguration_delete(changed);
    AConfiguration_delete(base);
    AConfiguration_delete(copy);
    AConfiguration_delete(config);
    return 0;
}
