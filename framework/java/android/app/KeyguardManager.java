/*
 * Ours under AOSP's name: android.app.KeyguardManager, the lock screen's
 * manager. AOSP's version builds a LockPatternUtils, asks the window manager, the trust manager and the notification
 * manager, none of which Minato has. Ours keeps its public API and answers as a phone does that has no secure lock
 * screen, the way the Android emulator (apk-runner-35) does: the keyguard is not showing, the device is not locked and
 * not secure, so a credential confirmation needs no intent (null), and dismissing the keyguard succeeds at once.
 *
 * That the device is not secure is a decision, not a gap: an app that finds a secure lock screen creates keys bound to
 * the user's authentication (setUserAuthenticationRequired) that nothing here could honour. A later bridge to the
 * iPhone's own authentication (LocalAuthentication) would change this answer together with BiometricManager's.
 */
package android.app;

import android.annotation.NonNull;
import android.annotation.Nullable;
import android.content.Context;
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.ArrayMap;
import android.util.Log;

import java.util.concurrent.Executor;

public class KeyguardManager {
    private static final String TAG = "KeyguardManager";

    /** The confirmation intent's action, with the extras of its title and description. */
    public static final String ACTION_CONFIRM_DEVICE_CREDENTIAL = "android.app.action.CONFIRM_DEVICE_CREDENTIAL";
    public static final String ACTION_CONFIRM_DEVICE_CREDENTIAL_WITH_USER =
            "android.app.action.CONFIRM_DEVICE_CREDENTIAL_WITH_USER";
    public static final String EXTRA_TITLE = "android.app.extra.TITLE";
    public static final String EXTRA_DESCRIPTION = "android.app.extra.DESCRIPTION";
    public static final String EXTRA_ALTERNATE_BUTTON_LABEL = "android.app.extra.ALTERNATE_BUTTON_LABEL";
    public static final String EXTRA_CHECKBOX_LABEL = "android.app.extra.CHECKBOX_LABEL";
    public static final String EXTRA_FORCE_TASK_OVERLAY = "android.app.KeyguardManager.FORCE_TASK_OVERLAY";
    public static final int RESULT_ALTERNATE = 1;

    private final Context mContext;
    private final ArrayMap<KeyguardLockedStateListener, Executor> mLockedStateListeners = new ArrayMap<>();

    KeyguardManager(Context context) {
        mContext = context;
    }

    /** A handle that disables and re-enables the keyguard; there is none to disable. */
    public class KeyguardLock {
        private final String mTag;

        KeyguardLock(String tag) {
            mTag = tag;
        }

        public void disableKeyguard() {
            Log.d(TAG, "disableKeyguard by " + mTag + ": no keyguard");
        }

        public void reenableKeyguard() {
            Log.d(TAG, "reenableKeyguard by " + mTag + ": no keyguard");
        }
    }

    /** Callback of {@link #exitKeyguardSecurely}. */
    public interface OnKeyguardExitResult {
        void onKeyguardExitResult(boolean success);
    }

    /** Callback of {@link #requestDismissKeyguard}. */
    public static abstract class KeyguardDismissCallback {
        public void onDismissError() { }

        public void onDismissSucceeded() { }

        public void onDismissCancelled() { }
    }

    /** Listener of the keyguard's locked state, which never changes here. */
    @FunctionalInterface
    public interface KeyguardLockedStateListener {
        void onKeyguardLockedStateChanged(boolean isKeyguardLocked);
    }

    /** @deprecated see {@link android.hardware.biometrics.BiometricPrompt.Builder#setAllowedAuthenticators(int)} */
    @Deprecated
    public Intent createConfirmDeviceCredentialIntent(CharSequence title, CharSequence description) {
        if (!isDeviceSecure()) return null;
        final Intent intent = new Intent(ACTION_CONFIRM_DEVICE_CREDENTIAL);
        intent.putExtra(EXTRA_TITLE, title);
        intent.putExtra(EXTRA_DESCRIPTION, description);
        return intent;
    }

    /** @deprecated Use {@link android.R.attr#showWhenLocked} or {@link Activity#setShowWhenLocked(boolean)}. */
    @Deprecated
    public KeyguardLock newKeyguardLock(String tag) {
        return new KeyguardLock(tag);
    }

    /** Whether the keyguard is showing: never. */
    public boolean isKeyguardLocked() {
        return false;
    }

    /** Whether the keyguard is secured by a PIN, pattern or password or a SIM card PIN: never. */
    public boolean isKeyguardSecure() {
        return false;
    }

    /** @deprecated Use {@link #isKeyguardLocked()}. */
    @Deprecated
    public boolean inKeyguardRestrictedInputMode() {
        return isKeyguardLocked();
    }

    /** Whether the device is locked: never. */
    public boolean isDeviceLocked() {
        return false;
    }

    /** Whether the device is secured with a PIN, pattern or password: never. */
    public boolean isDeviceSecure() {
        return false;
    }

    /** Requests the keyguard's dismissal, which succeeds at once, as when none is showing. */
    public void requestDismissKeyguard(@NonNull Activity activity, @Nullable KeyguardDismissCallback callback) {
        if (callback == null) return;
        new Handler(Looper.getMainLooper()).post(callback::onDismissSucceeded);
    }

    /** @deprecated Use {@link #requestDismissKeyguard}. */
    @Deprecated
    public void exitKeyguardSecurely(final OnKeyguardExitResult callback) {
        if (callback == null) return;
        callback.onKeyguardExitResult(true);
    }

    /** Keeps the listener; the keyguard's locked state never changes, so it is never called. */
    public void addKeyguardLockedStateListener(@NonNull Executor executor,
            @NonNull KeyguardLockedStateListener listener) {
        synchronized (mLockedStateListeners) {
            mLockedStateListeners.put(listener, executor);
        }
    }

    public void removeKeyguardLockedStateListener(@NonNull KeyguardLockedStateListener listener) {
        synchronized (mLockedStateListeners) {
            mLockedStateListeners.remove(listener);
        }
    }
}
