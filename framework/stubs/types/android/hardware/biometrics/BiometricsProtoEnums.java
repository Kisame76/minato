package android.hardware.biometrics;

/**
 * Compile-only stub (batch G), never in framework.jar: statsd's enum of where an enrollment started, which
 * BiometricFaceConstants and BiometricFingerprintConstants map their enroll reasons to for their metrics. javac inlines
 * the numbers, and nothing here reads a metric: the values are distinct, not AOSP's [unverified, the proto is not
 * fetched].
 */
public final class BiometricsProtoEnums {
    public static final int ENROLLMENT_SOURCE_UNKNOWN = 0;
    public static final int ENROLLMENT_SOURCE_FRR_NOTIFICATION = 1;
    public static final int ENROLLMENT_SOURCE_SETTINGS = 2;
    public static final int ENROLLMENT_SOURCE_SUW = 3;

    private BiometricsProtoEnums() {
    }
}
