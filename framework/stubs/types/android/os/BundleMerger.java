package android.os;

/** Compile-only stub (ADR-0039 step c2c1), never in framework.jar: Intent.mergeExtras merges with it (API 34). */
public class BundleMerger {
    public Bundle merge( Bundle first, Bundle last) {
        throw new RuntimeException("stub");
    }

    public static Object merge( int strategy, Object first, Object last) {
        throw new RuntimeException("stub");
    }
}
