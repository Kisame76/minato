package android.system.keystore2;

/**
 * Compile-only stub (batch G), never in framework.jar: the error codes of Keystore2's ResponseCode.aidl, which
 * android.security.KeyStoreException inlines to sort the codes the AndroidKeyStore provider hands it. No keystore
 * service runs here, so no code is ever made from them. The numbers are those of system/hardware/interfaces'
 * keystore2/aidl/android/system/keystore2/ResponseCode.aidl at android-15.0.0_r1 [verified: read online, every member
 * and number of the file; the rendered page has no line numbers].
 */
public final class ResponseCode {
    public static final int LOCKED = 2;
    public static final int UNINITIALIZED = 3;
    public static final int SYSTEM_ERROR = 4;
    public static final int PERMISSION_DENIED = 6;
    public static final int KEY_NOT_FOUND = 7;
    public static final int VALUE_CORRUPTED = 8;
    public static final int KEY_PERMANENTLY_INVALIDATED = 17;
    public static final int BACKEND_BUSY = 18;
    public static final int OPERATION_BUSY = 19;
    public static final int INVALID_ARGUMENT = 20;
    public static final int TOO_MUCH_DATA = 21;
    public static final int OUT_OF_KEYS = 22;
    public static final int OUT_OF_KEYS_REQUIRES_SYSTEM_UPGRADE = 23;
    public static final int OUT_OF_KEYS_PENDING_INTERNET_CONNECTIVITY = 24;
    public static final int OUT_OF_KEYS_TRANSIENT_ERROR = 25;
    public static final int OUT_OF_KEYS_PERMANENT_ERROR = 26;
    public static final int GET_ATTESTATION_APPLICATION_ID_FAILED = 27;

    private ResponseCode() {
    }
}
