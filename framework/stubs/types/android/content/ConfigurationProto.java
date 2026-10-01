package android.content;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: Configuration.dumpDebug and readFromProto name its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values, distinct
 * but not the proto's field numbers, are what dumpDebug writes and readFromProto expects; neither runs in an app.
 */
public final class ConfigurationProto {
    public static final long COLOR_MODE = 1L;
    public static final long DENSITY_DPI = 2L;
    public static final long FONT_SCALE = 3L;
    public static final long FONT_WEIGHT_ADJUSTMENT = 4L;
    public static final long GRAMMATICAL_GENDER = 5L;
    public static final long HARD_KEYBOARD_HIDDEN = 6L;
    public static final long KEYBOARD = 7L;
    public static final long KEYBOARD_HIDDEN = 8L;
    public static final long LOCALES = 9L;
    public static final long LOCALE_LIST = 10L;
    public static final long MCC = 11L;
    public static final long MNC = 12L;
    public static final long NAVIGATION = 13L;
    public static final long NAVIGATION_HIDDEN = 14L;
    public static final long ORIENTATION = 15L;
    public static final long SCREEN_HEIGHT_DP = 16L;
    public static final long SCREEN_LAYOUT = 17L;
    public static final long SCREEN_WIDTH_DP = 18L;
    public static final long SMALLEST_SCREEN_WIDTH_DP = 19L;
    public static final long TOUCHSCREEN = 20L;
    public static final long UI_MODE = 21L;
    public static final long WINDOW_CONFIGURATION = 22L;
}
