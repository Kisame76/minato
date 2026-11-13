/*
 * Ours under AOSP's name: the part of the AndroidKeyStore provider that
 * android.hardware.biometrics.CryptoObject asks, the keystore's operation handle of an initialized Signature, Cipher or
 * Mac, which a biometric prompt hands the authentication service to bind an authentication to one operation. Minato has
 * no AndroidKeyStore provider (no keystore service runs), so no primitive is backed by it: what AOSP does for a
 * primitive of another provider, the exception for the primitive that is not backed by it.
 */
package android.security.keystore2;

public abstract class AndroidKeyStoreProvider {
    private AndroidKeyStoreProvider() {
    }

    /** The keystore's operation handle of the crypto primitive, which only the AndroidKeyStore provider's have. */
    public static long getKeyStoreOperationHandle(Object cryptoPrimitive) {
        if (cryptoPrimitive == null) {
            throw new NullPointerException();
        }
        throw new IllegalArgumentException("Crypto primitive not backed by AndroidKeyStore provider: "
                + cryptoPrimitive + ", " + cryptoPrimitive.getClass());
    }
}
