package android.permission;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: PackageManager's caches name its property, a constant
 * javac inlines, so it has the tag's value. AttributionSource asks it whether a source with a token is registered
 * (ADR-0045).
 */
public final class PermissionManager {
    public static final String CACHE_KEY_PACKAGE_INFO = "cache_key.package_info";

    public boolean isRegisteredAttributionSource(android.content.AttributionSource source) {
        throw new RuntimeException("stub");
    }
}
