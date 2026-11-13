package android.app;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the proto of
 * core/proto/android/app/notificationmanager.proto, whose field IDs NotificationManager.Policy.dumpDebug writes;
 * javac inlines them. Each ID is ProtoStream's: count single (1 << 40) or repeated (2 << 40), the field's type <<
 * 32, its number.
 */
public final class PolicyProto {
    public static final long PRIORITY_CATEGORIES = 0x20e00000001L;
    public static final long PRIORITY_CALL_SENDER = 0x10e00000002L;
    public static final long PRIORITY_MESSAGE_SENDER = 0x10e00000003L;
    public static final long SUPPRESSED_VISUAL_EFFECTS = 0x20e00000004L;
}
