// The written part of ActivityOptions (the Unity test APK retest 10: Activity's methods name the type, and a class whose method
// types cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's methods. AOSP's
// class carries the system's window transitions (remote animations and transitions, window container tokens), which
// Minato's single activity window does not run, so the class is the API with the hidden members the taken files call:
// Activity's and ActivityTransitionState's scene transition info, PendingIntent's caller display and RemoteViews'
// pending intent launch, each keeping nothing, and IntentSender's background start mode, which its class
// initialisation sets on options of makeBasic and takes as a Bundle (ComponentOptions' key and rule), so that a class
// that holds an IntentSender, such as ClipData's items, loads (ADR-0057 step a).
import android.content.Intent;
import android.os.Bundle;
import android.os.ResultReceiver;
import java.util.ArrayList;

class ActivityOptions {
    /** @hide */
    public static final int MODE_BACKGROUND_ACTIVITY_START_COMPAT = -1;

    /** ComponentOptions' KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED. */
    private static final String KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED =
            "android.pendingIntent.backgroundActivityAllowed";

    private int mPendingIntentBackgroundActivityStartMode;
    private boolean mPendingIntentBackgroundActivityLaunchAllowedByPermission;

    public static ActivityOptions makeBasic() {
        return new ActivityOptions((Bundle) null);
    }

    public ActivityOptions setPendingIntentBackgroundActivityStartMode(int state) {
        mPendingIntentBackgroundActivityStartMode = state;
        return this;
    }

    public int getPendingIntentBackgroundActivityStartMode() {
        return mPendingIntentBackgroundActivityStartMode;
    }

    /** As ComponentOptions.toBundle: what differs from the defaults. */
    public Bundle toBundle() {
        Bundle b = new Bundle();
        if (mPendingIntentBackgroundActivityStartMode != 0) {
            b.putInt(KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED, mPendingIntentBackgroundActivityStartMode);
        }
        if (mPendingIntentBackgroundActivityLaunchAllowedByPermission) {
            b.putBoolean("android.pendingIntent.backgroundActivityAllowedByPermission", true);
        }
        return b;
    }

    /** @hide */
    public ActivityOptions(Bundle opts) {
    }

    /** @hide */
    public static class SceneTransitionInfo {
        /** @hide */
        public boolean isReturning() {
            return false;
        }

        /** @hide */
        public int getResultCode() {
            return 0;
        }

        /** @hide */
        public Intent getResultData() {
            return null;
        }

        /** @hide */
        public ArrayList<String> getSharedElementNames() {
            return null;
        }

        /** @hide */
        public ResultReceiver getResultReceiver() {
            return null;
        }

        /** @hide */
        public boolean isCrossTask() {
            return false;
        }

        /** @hide */
        public int getExitCoordinatorKey() {
            return 0;
        }
    }

    /** @hide */
    public ActivityOptions setSceneTransitionInfo(SceneTransitionInfo info) {
        return this;
    }

    /** @hide */
    public SceneTransitionInfo getSceneTransitionInfo() {
        return null;
    }

    /** @hide */
    public ActivityOptions setCallerDisplayId(int callerDisplayId) {
        return this;
    }

    /** @hide */
    public void setPendingIntentLaunchFlags(int flags) {
    }

    /** @hide */
    public void setPendingIntentBackgroundActivityLaunchAllowedByPermission(boolean allowed) {
        mPendingIntentBackgroundActivityLaunchAllowedByPermission = allowed;
    }
}
