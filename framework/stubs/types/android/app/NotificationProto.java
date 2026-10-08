package android.app;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: the proto of
 * core/proto/android/app/notification.proto, whose field IDs Notification.dumpDebug writes; javac inlines them. Each
 * ID is ProtoStream's: count single (1 << 40) or repeated (2 << 40), the field's type << 32, its number.
 */
public final class NotificationProto {
    public static final long CHANNEL_ID = 0x10900000001L;
    public static final long HAS_TICKER_TEXT = 0x10800000002L;
    public static final long FLAGS = 0x10500000003L;
    public static final long COLOR = 0x10500000004L;
    public static final long CATEGORY = 0x10900000005L;
    public static final long GROUP_KEY = 0x10900000006L;
    public static final long SORT_KEY = 0x10900000007L;
    public static final long ACTION_LENGTH = 0x10500000008L;
    public static final long VISIBILITY = 0x10e00000009L;
    public static final long PUBLIC_VERSION = 0x10b0000000aL;
}
