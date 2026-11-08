/*
 * The authentication service (gap 1 of the compatibility plan, batch G): IAuthService of system_server, local in the
 * app's process, which ServiceManager's table gives as "auth" and AOSP's BiometricPrompt asks. The iPhone has no
 * fingerprint, face or iris sensor an Android app could use (ADR-0045 decision 4 reports them as absent), so the
 * service answers as an Android 15 device without biometric hardware and without a secure lock screen: a prompt that
 * asks for a biometric ends in BIOMETRIC_ERROR_HW_NOT_PRESENT, one that asks for the device credential alone in
 * BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL [inferred from BiometricPrompt's error table, whose strings the receiver picks
 * for exactly these two codes], each delivered later on the main thread as the service delivers it, so that apps take
 * their fallback. The emulator's emulated sensor answers BIOMETRIC_ERROR_NO_BIOMETRICS instead: a named deviation.
 * Face ID through BiometricPrompt would be an ADR of its own (docs/plans/settings-candidates.md).
 */
package android.hardware.biometrics;

import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;

public final class LocalAuthService extends IAuthService.Default {
    private static final long FIRST_REQUEST = 1;
    private final Binder mBinder = new Binder();
    private long mRequests = FIRST_REQUEST;

    private LocalAuthService() {
        mBinder.attachInterface(this, IAuthService.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "auth": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalAuthService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public synchronized long authenticate(IBinder token, long sessionId, int userId,
            IBiometricServiceReceiver receiver, String opPackageName, PromptInfo promptInfo) {
        final int authenticators = promptInfo.getAuthenticators();
        final boolean credentialOnly = (authenticators & BiometricManager.Authenticators.DEVICE_CREDENTIAL) != 0
                && (authenticators & ~BiometricManager.Authenticators.DEVICE_CREDENTIAL) == 0;
        final int error = credentialOnly ? BiometricConstants.BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL
                : BiometricConstants.BIOMETRIC_ERROR_HW_NOT_PRESENT;
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                receiver.onError(BiometricAuthenticator.TYPE_NONE, error, 0 /* vendorCode */);
            } catch (RemoteException e) {
                // The receiver is in this process; it does not die.
            }
        });
        return mRequests++;
    }

    @Override
    public int canAuthenticate(String opPackageName, int userId, int authenticators) {
        return BiometricManager.canAuthenticateWithoutSensor(authenticators);
    }

    @Override
    public boolean hasEnrolledBiometrics(int userId, String opPackageName) {
        return false;
    }
}
