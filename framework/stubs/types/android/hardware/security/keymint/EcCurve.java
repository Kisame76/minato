package android.hardware.security.keymint;

/**
 * Compile-only stub (batch D 2), never in framework.jar: KeyMint's EC curves, which KeyProperties inlines in its
 * conversions for ADR-0059's provider; the values are the AIDL enum's at android-15.0.0_r1 (hardware/interfaces
 * security/keymint EcCurve.aidl), whose AIDL is not fetched yet.
 */
public final class EcCurve {
    public static final int P_224 = 0;
    public static final int P_256 = 1;
    public static final int P_384 = 2;
    public static final int P_521 = 3;
    public static final int CURVE_25519 = 4;

    private EcCurve() {
    }
}
