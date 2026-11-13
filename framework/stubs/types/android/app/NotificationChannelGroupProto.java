package android.app;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the proto of
 * core/proto/android/app/notification_channel_group.proto, whose field IDs NotificationChannelGroup.dumpDebug
 * writes; javac inlines them. Each ID is ProtoStream's: count single (1 << 40) or repeated (2 << 40), the field's
 * type << 32, its number.
 */
public final class NotificationChannelGroupProto {
    public static final long ID = 0x10900000001L;
    public static final long NAME = 0x10900000002L;
    public static final long DESCRIPTION = 0x10900000003L;
    public static final long IS_BLOCKED = 0x10800000004L;
    public static final long CHANNELS = 0x20b00000005L;
}
