package android.content.pm;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: a profile's properties, which ContentProvider asks
 * for a cross-profile caller's media.
 */
public final class UserProperties {
    public boolean isMediaSharedWithParent() {
        throw new RuntimeException("stub");
    }
}
