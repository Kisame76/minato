// The written part of MediaDrm (the Unity test APK gap sort, Formate's category 2, android.media as absent hardware), which
// tools/managergen joins with the API's methods. Minato has no DRM plugin: no scheme is supported, so the constructor
// throws what AOSP's native setup throws for a scheme with no plugin, and the queries say no, as a device does that
// has no DRM HAL.
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

class MediaDrm {
    public MediaDrm(UUID uuid) throws UnsupportedSchemeException {
        throw new UnsupportedSchemeException("Failed to instantiate drm object: NO_INIT");
    }

    public static final boolean isCryptoSchemeSupported(UUID uuid) {
        return false;
    }

    public static final boolean isCryptoSchemeSupported(UUID uuid, String mimeType) {
        return false;
    }

    public static final boolean isCryptoSchemeSupported(UUID uuid, String mimeType, int securityLevel) {
        return false;
    }

    public static final List<UUID> getSupportedCryptoSchemes() {
        return new ArrayList<>();
    }
}
