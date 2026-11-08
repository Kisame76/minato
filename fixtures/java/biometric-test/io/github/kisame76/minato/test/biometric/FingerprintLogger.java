package io.github.kisame76.minato.test.biometric;

import android.hardware.fingerprint.FingerprintManager;
import android.util.Log;

/** The legacy fingerprint manager's authentication, which ends in an error on a device without an enrolled finger. */
public class FingerprintLogger extends FingerprintManager.AuthenticationCallback {
    @SuppressWarnings("deprecation")
    void authenticate(Object manager) {
        ((FingerprintManager) manager).authenticate(null, null, 0, this, null);
        Log.i("biometric-test", "FingerprintManager.authenticate returned");
    }

    @Override
    public void onAuthenticationError(int errorCode, CharSequence errString) {
        // The emulator's emulated sensor gives no error within the test's time; a device without one ends in the
        // error for no hardware (12): only shown.
        Log.i("biometric-test-device", "FingerprintManager: onAuthenticationError " + errorCode + " \"" + errString + "\"");
    }
}
