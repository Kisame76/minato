package android.app;

/**
 * Compile-only stub (ADR-0045 step d2), never in framework.jar: PendingIntent's dumpDebug names its field constant,
 * which protoc's javastream plugin generates from core/proto. javac inlines it, so this value, distinct but not the
 * proto's field number, is what dumpDebug writes, as BundleProto's; it does not run in an app.
 */
public final class PendingIntentProto {
    public static final long TARGET = 1L;
}
