/*
 * Ours under AOSP's name: android.hardware.biometrics.BiometricManager.
 * AOSP's asks the authentication service (IAuthService), whose interface reaches the fingerprint, face and iris
 * services' types by the hundred; Minato has none of them. Ours keeps the public API apps and the AndroidX biometric
 * library call and answers as a phone without a biometric sensor: canAuthenticate gives BIOMETRIC_ERROR_NO_HARDWARE
 * for biometrics. A later bridge to the iPhone's own authentication (LocalAuthentication) would change this answer.
 */
package android.hardware.biometrics;

import android.annotation.IntDef;
import android.content.Context;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

public class BiometricManager {
    public static final int BIOMETRIC_SUCCESS = 0;
    public static final int BIOMETRIC_ERROR_HW_UNAVAILABLE = 1;
    public static final int BIOMETRIC_ERROR_NONE_ENROLLED = 11;
    public static final int BIOMETRIC_ERROR_NO_HARDWARE = 12;
    public static final int BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED = 15;
    public static final long BIOMETRIC_NO_AUTHENTICATION = 0;
    public static final String EXTRA_ENROLL_REASON = "enroll_reason";

    @IntDef({BIOMETRIC_SUCCESS, BIOMETRIC_ERROR_HW_UNAVAILABLE, BIOMETRIC_ERROR_NONE_ENROLLED,
            BIOMETRIC_ERROR_NO_HARDWARE, BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED})
    @Retention(RetentionPolicy.SOURCE)
    public @interface BiometricError {}

    /** The authenticator types a caller may combine. */
    public interface Authenticators {
        int EMPTY_SET = 0x0000;
        int BIOMETRIC_MAX_STRENGTH = 0x0001;
        int BIOMETRIC_STRONG = 0x000F;
        int BIOMETRIC_WEAK = 0x00FF;
        int BIOMETRIC_CONVENIENCE = 0x0FFF;
        int BIOMETRIC_MIN_STRENGTH = 0x7FFF;
        int DEVICE_CREDENTIAL = 1 << 15;

        @IntDef(flag = true, value = {BIOMETRIC_MAX_STRENGTH, BIOMETRIC_STRONG, BIOMETRIC_WEAK, BIOMETRIC_CONVENIENCE,
                DEVICE_CREDENTIAL})
        @Retention(RetentionPolicy.SOURCE)
        @interface Types {}
    }

    /** Strings a prompt shows for a set of authenticators; none without a sensor. */
    public static class Strings {
        Strings() {
        }

        public CharSequence getButtonLabel() {
            return null;
        }

        public CharSequence getPromptMessage() {
            return null;
        }

        public CharSequence getSettingName() {
            return null;
        }
    }

    private final Context mContext;

    public BiometricManager(Context context) {
        mContext = context;
    }

    /** @deprecated Use {@link #canAuthenticate(int)}. */
    @Deprecated
    @BiometricError
    public int canAuthenticate() {
        return canAuthenticate(Authenticators.BIOMETRIC_WEAK);
    }

    /** Whether the user can authenticate with the given authenticators: not with a biometric, nothing to enroll. */
    @BiometricError
    public int canAuthenticate(int authenticators) {
        // As the authentication service checks (apk-runner-35's message): the app must hold USE_BIOMETRIC.
        if (mContext.checkSelfPermission("android.permission.USE_BIOMETRIC")
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Must have USE_BIOMETRIC permission: Neither user " + android.os.Process.myUid()
                    + " nor current process has android.permission.USE_BIOMETRIC.");
        }
        return canAuthenticateWithoutSensor(authenticators);
    }

    /**
     * What the authentication service answers a device without a biometric sensor and without a secure lock screen, which
     * is what an iPhone is to Android code: an authenticator combination it does not take is a SecurityException (the
     * emulator's answer for 0x7fff, 0x1 and 0x2 [verified: batch-g-test]; the rule is BiometricService's, of which the
     * sources fetched here have only the hidden valid combinations [inferred]: no biometric bits, STRONG or WEAK, with
     * or without DEVICE_CREDENTIAL); a device credential alone is not set, a biometric has no hardware
     * (BIOMETRIC_ERROR_NO_HARDWARE, where the emulator's emulated sensor says none enrolled [deviation, ADR-0045
     * decision 4: the iPhone has none]).
     */
    static int canAuthenticateWithoutSensor(int authenticators) {
        final int biometric = authenticators & ~Authenticators.DEVICE_CREDENTIAL;
        if (biometric != 0 && biometric != Authenticators.BIOMETRIC_STRONG && biometric != Authenticators.BIOMETRIC_WEAK) {
            throw new SecurityException("Invalid authenticator configuration: " + Integer.toHexString(authenticators));
        }
        if ((authenticators & Authenticators.DEVICE_CREDENTIAL) != 0 && biometric == 0) {
            // A device credential alone: none is set on a device with no secure lock screen (see KeyguardManager).
            return BIOMETRIC_ERROR_NONE_ENROLLED;
        }
        return BIOMETRIC_ERROR_NO_HARDWARE;
    }

    /** The strings for the authenticators: none without a sensor. */
    public Strings getStrings(int authenticators) {
        return null;
    }
}
