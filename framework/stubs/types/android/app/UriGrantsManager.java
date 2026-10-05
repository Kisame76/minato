package android.app;

/**
 * Compile-only stub (ADR-0045, gap 13a), never in framework.jar: ContentResolver's persisted URI permissions ask the
 * URI grants service through it, which Minato has none of.
 */
public class UriGrantsManager {
    public static IUriGrantsManager getService() {
        throw new RuntimeException("stub");
    }
}
