package android.hardware.face;

import android.content.Context;

/**
 * Compile-only stub (batch G), never in framework.jar: the face service's manager, whose static error text
 * BiometricPrompt's receiver asks for an error of the face modality. Minato has no face sensor an app can use
 * (ADR-0045 decision 4), so no service sends that modality.
 */
public class FaceManager {
    private FaceManager() {
    }

    public static String getErrorString(Context context, int errMsg, int vendorCode) {
        return null;
    }
}
