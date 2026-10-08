// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Activity names the scene transition info. (ADR-0039 step c2c3) Activity's scene transitions. (ADR-0039 step c2c3)
// ActivityTransitionState reads a scene transition's info. (ADR-0045 step d2) PendingIntent's activities name their
// caller's display. (ADR-0045 gap 1, batch 2) RemoteViews' clicks set a pending intent's launch.
import android.content.Intent;
import android.os.Bundle;
import android.os.ResultReceiver;
import java.util.ArrayList;

class ActivityOptions {
    /** @hide */
    public ActivityOptions(Bundle opts) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static class SceneTransitionInfo {
        /** @hide */
        public boolean isReturning() { throw new RuntimeException("stub"); }
        /** @hide */
        public int getResultCode() { throw new RuntimeException("stub"); }
        /** @hide */
        public Intent getResultData() { throw new RuntimeException("stub"); }
        /** @hide */
        public ArrayList<String> getSharedElementNames() { throw new RuntimeException("stub"); }
        /** @hide */
        public ResultReceiver getResultReceiver() { throw new RuntimeException("stub"); }
        /** @hide */
        public boolean isCrossTask() { throw new RuntimeException("stub"); }
        /** @hide */
        public int getExitCoordinatorKey() { throw new RuntimeException("stub"); }
    }

    /** @hide */
    public ActivityOptions setSceneTransitionInfo(SceneTransitionInfo info) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public SceneTransitionInfo getSceneTransitionInfo() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public ActivityOptions setCallerDisplayId(int callerDisplayId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setPendingIntentLaunchFlags(int flags) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setPendingIntentBackgroundActivityLaunchAllowedByPermission(boolean allowed) {
        throw new RuntimeException("stub");
    }
}
