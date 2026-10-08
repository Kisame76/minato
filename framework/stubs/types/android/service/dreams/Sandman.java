package android.service.dreams;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 1), never in framework.jar: the dream starter of PowerManager.dream, a
 * system API that needs WRITE_DREAM_STATE, which no app holds; Minato has no screen saver.
 */
public final class Sandman {
    public static void startDreamByUserRequest(android.content.Context context) {
        throw new RuntimeException("stub");
    }
}
