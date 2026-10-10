// The written part of MediaCrypto (the Unity test APK gap sort, Formate's category 2): with no DRM plugin (see MediaDrm), no
// scheme is supported and a crypto object cannot be made.
import java.util.UUID;

class MediaCrypto {
    public MediaCrypto(UUID uuid, byte[] sessionId) throws MediaCryptoException {
        throw new MediaCryptoException("Failed to instantiate crypto object: NO_INIT");
    }

    public static final boolean isCryptoSchemeSupported(UUID uuid) {
        return false;
    }
}
