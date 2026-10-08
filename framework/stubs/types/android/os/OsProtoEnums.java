package android.os;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 1), never in framework.jar: the proto enums of os/enums.proto, whose wake
 * lock levels PowerManager's level constants take as theirs; javac inlines the values, as AOSP's build does.
 */
public final class OsProtoEnums {
    public static final int PARTIAL_WAKE_LOCK = 1;
    public static final int SCREEN_DIM_WAKE_LOCK = 6;
    public static final int SCREEN_BRIGHT_WAKE_LOCK = 10;
    public static final int FULL_WAKE_LOCK = 26;
    public static final int PROXIMITY_SCREEN_OFF_WAKE_LOCK = 32;
    public static final int DOZE_WAKE_LOCK = 64;
    public static final int DRAW_WAKE_LOCK = 128;
    public static final int SCREEN_TIMEOUT_OVERRIDE_WAKE_LOCK = 256;
}
