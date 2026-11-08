package android.security.keymaster;

/**
 * Compile-only stub (batch D 2), never in framework.jar: the KeyMint constants KeyProperties inlines in its
 * conversions to and from KeyMint, which only ADR-0059's provider calls. The values are KeyMint's AIDL enums at
 * android-15.0.0_r1 (hardware/interfaces security/keymint: Algorithm, BlockMode, Digest, PaddingMode, KeyPurpose,
 * KeyOrigin, SecurityLevel), which AOSP's KeymasterDefs takes; their AIDL is not fetched yet.
 */
public final class KeymasterDefs {
    public static final int KM_ALGORITHM_RSA = 1;
    public static final int KM_ALGORITHM_EC = 3;
    public static final int KM_ALGORITHM_AES = 32;
    public static final int KM_ALGORITHM_3DES = 33;
    public static final int KM_ALGORITHM_HMAC = 128;

    public static final int KM_MODE_ECB = 1;
    public static final int KM_MODE_CBC = 2;
    public static final int KM_MODE_CTR = 3;
    public static final int KM_MODE_GCM = 32;

    public static final int KM_PAD_NONE = 1;
    public static final int KM_PAD_RSA_OAEP = 2;
    public static final int KM_PAD_RSA_PSS = 3;
    public static final int KM_PAD_RSA_PKCS1_1_5_ENCRYPT = 4;
    public static final int KM_PAD_RSA_PKCS1_1_5_SIGN = 5;
    public static final int KM_PAD_PKCS7 = 64;

    public static final int KM_DIGEST_NONE = 0;
    public static final int KM_DIGEST_MD5 = 1;
    public static final int KM_DIGEST_SHA1 = 2;
    public static final int KM_DIGEST_SHA_2_224 = 3;
    public static final int KM_DIGEST_SHA_2_256 = 4;
    public static final int KM_DIGEST_SHA_2_384 = 5;
    public static final int KM_DIGEST_SHA_2_512 = 6;

    public static final int KM_PURPOSE_ENCRYPT = 0;
    public static final int KM_PURPOSE_DECRYPT = 1;
    public static final int KM_PURPOSE_SIGN = 2;
    public static final int KM_PURPOSE_VERIFY = 3;
    public static final int KM_PURPOSE_WRAP = 5;
    public static final int KM_PURPOSE_AGREE_KEY = 6;
    public static final int KM_PURPOSE_ATTEST_KEY = 7;

    public static final int KM_ORIGIN_GENERATED = 0;
    public static final int KM_ORIGIN_IMPORTED = 2;
    public static final int KM_ORIGIN_UNKNOWN = 3;
    public static final int KM_ORIGIN_SECURELY_IMPORTED = 4;

    public static final int KM_SECURITY_LEVEL_SOFTWARE = 0;
    public static final int KM_SECURITY_LEVEL_TRUSTED_ENVIRONMENT = 1;
    public static final int KM_SECURITY_LEVEL_STRONGBOX = 2;

    // The KeyMint error codes KeyStoreException sorts (hardware/interfaces security/keymint/aidl/android/hardware/security/keymint/ErrorCode.aidl at android-15.0.0_r1
    // [verified: read online, every one of the 86 numbers below against the file; no line numbers in the rendered page].
    public static final int KM_ERROR_ATTESTATION_APPLICATION_ID_MISSING = -65;
    public static final int KM_ERROR_ATTESTATION_CHALLENGE_MISSING = -63;
    public static final int KM_ERROR_ATTESTATION_IDS_NOT_PROVISIONED = -75;
    public static final int KM_ERROR_ATTESTATION_KEYS_NOT_PROVISIONED = -74;
    public static final int KM_ERROR_BOOT_LEVEL_EXCEEDED = -84;
    public static final int KM_ERROR_CALLER_NONCE_PROHIBITED = -55;
    public static final int KM_ERROR_CANNOT_ATTEST_IDS = -66;
    public static final int KM_ERROR_CONCURRENT_ACCESS_CONFLICT = -47;
    public static final int KM_ERROR_CONCURRENT_PROOF_OF_PRESENCE_REQUESTED = -70;
    public static final int KM_ERROR_DELEGATION_NOT_ALLOWED = -23;
    public static final int KM_ERROR_DEVICE_LOCKED = -72;
    public static final int KM_ERROR_EARLY_BOOT_ENDED = -73;
    public static final int KM_ERROR_HARDWARE_NOT_YET_AVAILABLE = -85;
    public static final int KM_ERROR_HARDWARE_TYPE_UNAVAILABLE = -68;
    public static final int KM_ERROR_IMPORTED_KEY_DECRYPTION_FAILED = -35;
    public static final int KM_ERROR_IMPORTED_KEY_NOT_ENCRYPTED = -34;
    public static final int KM_ERROR_IMPORTED_KEY_NOT_SIGNED = -36;
    public static final int KM_ERROR_IMPORTED_KEY_VERIFICATION_FAILED = -37;
    public static final int KM_ERROR_IMPORT_PARAMETER_MISMATCH = -44;
    public static final int KM_ERROR_INCOMPATIBLE_ALGORITHM = -5;
    public static final int KM_ERROR_INCOMPATIBLE_BLOCK_MODE = -8;
    public static final int KM_ERROR_INCOMPATIBLE_DIGEST = -13;
    public static final int KM_ERROR_INCOMPATIBLE_KEY_FORMAT = -18;
    public static final int KM_ERROR_INCOMPATIBLE_MGF_DIGEST = -78;
    public static final int KM_ERROR_INCOMPATIBLE_PADDING_MODE = -11;
    public static final int KM_ERROR_INCOMPATIBLE_PURPOSE = -3;
    public static final int KM_ERROR_INSUFFICIENT_BUFFER_SPACE = -29;
    public static final int KM_ERROR_INVALID_ARGUMENT = -38;
    public static final int KM_ERROR_INVALID_AUTHORIZATION_TIMEOUT = -16;
    public static final int KM_ERROR_INVALID_EXPIRATION_TIME = -14;
    public static final int KM_ERROR_INVALID_INPUT_LENGTH = -21;
    public static final int KM_ERROR_INVALID_ISSUER_SUBJECT = -83;
    public static final int KM_ERROR_INVALID_KEY_BLOB = -33;
    public static final int KM_ERROR_INVALID_MAC_LENGTH = -57;
    public static final int KM_ERROR_INVALID_NONCE = -52;
    public static final int KM_ERROR_INVALID_OPERATION = -76;
    public static final int KM_ERROR_INVALID_OPERATION_HANDLE = -28;
    public static final int KM_ERROR_INVALID_TAG = -40;
    public static final int KM_ERROR_INVALID_USER_ID = -15;
    public static final int KM_ERROR_KEYMINT_NOT_CONFIGURED = -64;
    public static final int KM_ERROR_KEY_EXPIRED = -25;
    public static final int KM_ERROR_KEY_EXPORT_OPTIONS_INVALID = -22;
    public static final int KM_ERROR_KEY_MAX_OPS_EXCEEDED = -56;
    public static final int KM_ERROR_KEY_NOT_YET_VALID = -24;
    public static final int KM_ERROR_KEY_RATE_LIMIT_EXCEEDED = -54;
    public static final int KM_ERROR_KEY_USER_NOT_AUTHENTICATED = -26;
    public static final int KM_ERROR_MEMORY_ALLOCATION_FAILED = -41;
    public static final int KM_ERROR_MISSING_ISSUER_SUBJECT = -82;
    public static final int KM_ERROR_MISSING_MAC_LENGTH = -53;
    public static final int KM_ERROR_MISSING_MIN_MAC_LENGTH = -58;
    public static final int KM_ERROR_MISSING_NONCE = -51;
    public static final int KM_ERROR_MISSING_NOT_AFTER = -81;
    public static final int KM_ERROR_MISSING_NOT_BEFORE = -80;
    public static final int KM_ERROR_NO_USER_CONFIRMATION = -71;
    public static final int KM_ERROR_OK = 0;
    public static final int KM_ERROR_OPERATION_CANCELLED = -46;
    public static final int KM_ERROR_OUTPUT_PARAMETER_NULL = -27;
    public static final int KM_ERROR_PROOF_OF_PRESENCE_REQUIRED = -69;
    public static final int KM_ERROR_ROLLBACK_RESISTANCE_UNAVAILABLE = -67;
    public static final int KM_ERROR_ROOT_OF_TRUST_ALREADY_SET = -1;
    public static final int KM_ERROR_SECURE_HW_ACCESS_DENIED = -45;
    public static final int KM_ERROR_SECURE_HW_BUSY = -48;
    public static final int KM_ERROR_SECURE_HW_COMMUNICATION_FAILED = -49;
    public static final int KM_ERROR_STORAGE_KEY_UNSUPPORTED = -77;
    public static final int KM_ERROR_TOO_MANY_OPERATIONS = -31;
    public static final int KM_ERROR_UNEXPECTED_NULL_POINTER = -32;
    public static final int KM_ERROR_UNIMPLEMENTED = -100;
    public static final int KM_ERROR_UNKNOWN_ERROR = -1000;
    public static final int KM_ERROR_UNSUPPORTED_ALGORITHM = -4;
    public static final int KM_ERROR_UNSUPPORTED_BLOCK_MODE = -7;
    public static final int KM_ERROR_UNSUPPORTED_DIGEST = -12;
    public static final int KM_ERROR_UNSUPPORTED_EC_CURVE = -61;
    public static final int KM_ERROR_UNSUPPORTED_EC_FIELD = -50;
    public static final int KM_ERROR_UNSUPPORTED_KDF = -60;
    public static final int KM_ERROR_UNSUPPORTED_KEY_ENCRYPTION_ALGORITHM = -19;
    public static final int KM_ERROR_UNSUPPORTED_KEY_FORMAT = -17;
    public static final int KM_ERROR_UNSUPPORTED_KEY_SIZE = -6;
    public static final int KM_ERROR_UNSUPPORTED_KEY_VERIFICATION_ALGORITHM = -20;
    public static final int KM_ERROR_UNSUPPORTED_MAC_LENGTH = -9;
    public static final int KM_ERROR_UNSUPPORTED_MGF_DIGEST = -79;
    public static final int KM_ERROR_UNSUPPORTED_MIN_MAC_LENGTH = -59;
    public static final int KM_ERROR_UNSUPPORTED_PADDING_MODE = -10;
    public static final int KM_ERROR_UNSUPPORTED_PURPOSE = -2;
    public static final int KM_ERROR_UNSUPPORTED_TAG = -39;
    public static final int KM_ERROR_VERIFICATION_FAILED = -30;
    public static final int KM_ERROR_VERSION_MISMATCH = -101;

    private KeymasterDefs() {
    }
}
