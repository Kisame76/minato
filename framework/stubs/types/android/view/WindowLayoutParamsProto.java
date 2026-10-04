package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: WindowManager.LayoutParams.dumpDebug names its field
 * constants, which protoc's javastream plugin generates from core/proto. javac inlines them, so these values,
 * distinct but not the proto's field numbers, are what dumpDebug writes; it never runs in an app.
 */
public final class WindowLayoutParamsProto {
    public static final long ALPHA = 1L;
    public static final long APPEARANCE = 2L;
    public static final long BEHAVIOR = 3L;
    public static final long BUTTON_BRIGHTNESS = 4L;
    public static final long COLOR_MODE = 5L;
    public static final long FIT_IGNORE_VISIBILITY = 6L;
    public static final long FIT_INSETS_SIDES = 7L;
    public static final long FIT_INSETS_TYPES = 8L;
    public static final long FLAGS = 9L;
    public static final long FORMAT = 10L;
    public static final long GRAVITY = 11L;
    public static final long HAS_SYSTEM_UI_LISTENERS = 12L;
    public static final long HEIGHT = 13L;
    public static final long HORIZONTAL_MARGIN = 14L;
    public static final long INPUT_FEATURE_FLAGS = 15L;
    public static final long PREFERRED_REFRESH_RATE = 16L;
    public static final long PRIVATE_FLAGS = 17L;
    public static final long ROTATION_ANIMATION = 18L;
    public static final long SCREEN_BRIGHTNESS = 19L;
    public static final long SOFT_INPUT_MODE = 20L;
    public static final long SUBTREE_SYSTEM_UI_VISIBILITY_FLAGS = 21L;
    public static final long SYSTEM_UI_VISIBILITY_FLAGS = 22L;
    public static final long TYPE = 23L;
    public static final long USER_ACTIVITY_TIMEOUT = 24L;
    public static final long VERTICAL_MARGIN = 25L;
    public static final long WIDTH = 26L;
    public static final long WINDOW_ANIMATIONS = 27L;
    public static final long X = 28L;
    public static final long Y = 29L;
    public static final long PREFERRED_DISPLAY_MODE_ID = 31L;
}
