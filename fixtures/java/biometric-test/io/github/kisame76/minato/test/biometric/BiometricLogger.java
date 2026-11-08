package io.github.kisame76.minato.test.biometric;

import android.hardware.biometrics.BiometricPrompt;
import android.util.Log;

/** An authentication callback that logs what it is told: on a device without a usable biometric, an error. */
public class BiometricLogger extends BiometricPrompt.AuthenticationCallback {
    @Override
    public void onAuthenticationError(int errorCode, CharSequence errString) {
        Log.i("biometric-test", "BiometricPrompt: onAuthenticationError, message "
                + (errString != null && errString.length() > 0));
        // The emulator's emulated sensor says nothing is enrolled (11, "Face Unlock"); a device without one says there
        // is no hardware (12).
        Log.i("biometric-test-device", "BiometricPrompt: onAuthenticationError " + errorCode + " \"" + errString + "\"");
    }

    @Override
    public void onAuthenticationHelp(int helpCode, CharSequence helpString) {
        Log.i("biometric-test", "BiometricPrompt: onAuthenticationHelp");
    }

    @Override
    public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result) {
        Log.i("biometric-test", "BiometricPrompt: onAuthenticationSucceeded");
    }

    @Override
    public void onAuthenticationFailed() {
        Log.i("biometric-test", "BiometricPrompt: onAuthenticationFailed");
    }
}
