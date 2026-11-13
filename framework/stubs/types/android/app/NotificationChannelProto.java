package android.app;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the proto of
 * core/proto/android/app/notification_channel.proto, whose field IDs NotificationChannel.dumpDebug writes; javac
 * inlines them. Each ID is ProtoStream's: count single (1 << 40) or repeated (2 << 40), the field's type << 32, its
 * number.
 */
public final class NotificationChannelProto {
    public static final long ID = 0x10900000001L;
    public static final long NAME = 0x10900000002L;
    public static final long DESCRIPTION = 0x10900000003L;
    public static final long IMPORTANCE = 0x10500000004L;
    public static final long CAN_BYPASS_DND = 0x10800000005L;
    public static final long LOCKSCREEN_VISIBILITY = 0x10500000006L;
    public static final long SOUND = 0x10900000007L;
    public static final long USE_LIGHTS = 0x10800000008L;
    public static final long LIGHT_COLOR = 0x10500000009L;
    public static final long VIBRATION = 0x2030000000aL;
    public static final long USER_LOCKED_FIELDS = 0x1050000000bL;
    public static final long IS_VIBRATION_ENABLED = 0x1080000000cL;
    public static final long SHOW_BADGE = 0x1080000000dL;
    public static final long IS_DELETED = 0x1080000000eL;
    public static final long GROUP = 0x1090000000fL;
    public static final long AUDIO_ATTRIBUTES = 0x10b00000010L;
    public static final long IS_BLOCKABLE_SYSTEM = 0x10800000011L;
    public static final long USER_VISIBLE_TASK_SHOWN = 0x10800000012L;
    public static final long ALLOW_APP_OVERLAY = 0x10800000013L;
}
