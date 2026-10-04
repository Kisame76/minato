package android.view;

/**
 * Compile-only stub (ADR-0041 a)), never in framework.jar: Display's state and state-reason constants name it, which
 * proto_logging's stats/enums/view/enums.proto generates. javac inlines them, so these are the proto's values (Android
 * 15).
 */
public final class ViewProtoEnums {
    public static final int DISPLAY_STATE_UNKNOWN = 0;
    public static final int DISPLAY_STATE_OFF = 1;
    public static final int DISPLAY_STATE_ON = 2;
    public static final int DISPLAY_STATE_DOZE = 3;
    public static final int DISPLAY_STATE_DOZE_SUSPEND = 4;
    public static final int DISPLAY_STATE_VR = 5;
    public static final int DISPLAY_STATE_ON_SUSPEND = 6;
    public static final int DISPLAY_STATE_REASON_UNKNOWN = 0;
    public static final int DISPLAY_STATE_REASON_DEFAULT_POLICY = 1;
    public static final int DISPLAY_STATE_REASON_DRAW_WAKE_LOCK = 2;
    public static final int DISPLAY_STATE_REASON_OFFLOAD = 3;
    public static final int DISPLAY_STATE_REASON_TILT = 4;
    public static final int DISPLAY_STATE_REASON_DREAM_MANAGER = 5;
    public static final int DISPLAY_STATE_REASON_KEY = 6;
    public static final int DISPLAY_STATE_REASON_MOTION = 7;
}
