// The written part of FingerprintManager (services batch C): the fingerprint reader, of which Minato reports none. An
// app that holds neither USE_FINGERPRINT nor USE_BIOMETRIC gets the SecurityException Android 15 gives it; one that
// holds one is told that there is no hardware and no enrolled finger, as on the emulator
// [verified: services-c-test for the exception, AOSP's FingerprintManager for the rest]. Authentication with the
// iPhone's own sensor would be a bridge to LocalAuthentication, together with BiometricManager's.
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;

import java.security.Signature;

import javax.crypto.Cipher;
import javax.crypto.Mac;

class FingerprintManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public FingerprintManager(Context context) {
        mContext = context;
    }

    private void checkPermission(String what) {
        if (mContext.checkSelfPermission("android.permission.USE_FINGERPRINT") == PackageManager.PERMISSION_GRANTED
                || mContext.checkSelfPermission("android.permission.USE_BIOMETRIC")
                == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        throw new SecurityException(what + ": Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.USE_FINGERPRINT.");
    }

    public boolean isHardwareDetected() {
        checkPermission("isHardwareDetected");
        return false;
    }

    public boolean hasEnrolledFingerprints() {
        checkPermission("hasEnrolledFingerprints");
        return false;
    }

    public void authenticate(CryptoObject crypto, CancellationSignal cancel, int flags,
            AuthenticationCallback callback, Handler handler) {
        if (callback == null) {
            throw new IllegalArgumentException("Must supply an authentication callback");
        }
        checkPermission("authenticate");
        if (cancel != null && cancel.isCanceled()) {
            return;
        }
        final Handler target = handler != null ? handler : new Handler(Looper.getMainLooper());
        target.post(() -> callback.onAuthenticationError(FINGERPRINT_ERROR_HW_NOT_PRESENT,
                "Fingerprint hardware not present"));
    }

    /** The callback's methods do nothing unless an app overrides them. */
    public abstract static class AuthenticationCallback {
        public void onAuthenticationError(int errorCode, CharSequence errString) { }

        public void onAuthenticationHelp(int helpCode, CharSequence helpString) { }

        public void onAuthenticationSucceeded(AuthenticationResult result) { }

        public void onAuthenticationFailed() { }
    }

    public static class AuthenticationResult {
        private final CryptoObject mCryptoObject;

        /** @hide */
        public AuthenticationResult(CryptoObject crypto) {
            mCryptoObject = crypto;
        }

        public CryptoObject getCryptoObject() {
            return mCryptoObject;
        }
    }

    public static final class CryptoObject {
        private final Signature mSignature;
        private final Cipher mCipher;
        private final Mac mMac;

        public CryptoObject(Signature signature) {
            mSignature = signature;
            mCipher = null;
            mMac = null;
        }

        public CryptoObject(Cipher cipher) {
            mCipher = cipher;
            mSignature = null;
            mMac = null;
        }

        public CryptoObject(Mac mac) {
            mMac = mac;
            mCipher = null;
            mSignature = null;
        }

        public Signature getSignature() {
            return mSignature;
        }

        public Cipher getCipher() {
            return mCipher;
        }

        public Mac getMac() {
            return mMac;
        }
    }
}
