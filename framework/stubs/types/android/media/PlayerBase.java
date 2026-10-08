package android.media;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: the players' base class, whose stream type check
 * Notification.Builder's deprecated setSound(Uri, int) asks.
 */
public abstract class PlayerBase {
    public static void deprecateStreamTypeForPlayback(int streamType, String className, String opName) {
        throw new RuntimeException("stub");
    }
}
