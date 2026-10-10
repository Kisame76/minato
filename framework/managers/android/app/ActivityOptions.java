// The written part of ActivityOptions (the Unity test APK retest 10: Activity's methods name the type, and a class whose method
// types cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's methods. AOSP's
// class carries the system's window transitions (remote animations and transitions, window container tokens), which
// Minato's single activity window does not run, so the class is the API with the hidden members the taken files call:
// Activity's and ActivityTransitionState's scene transition info, PendingIntent's caller display and RemoteViews'
// pending intent launch, each keeping nothing, and IntentSender's background start mode.
import android.content.Intent;
import android.os.Bundle;
import android.os.ResultReceiver;
import java.util.ArrayList;

class ActivityOptions {
    /** @hide */
    public static final int MODE_BACKGROUND_ACTIVITY_START_COMPAT = -1;

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
    }
}
