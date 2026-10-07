/*
 * The activity client controller of Minato's one app process (ADR-0039 step c2c3, c2c3's decision 1): what
 * ActivityClient asks system_server about an activity, answered in the process. It is AIDL's Default class with
 * Minato's answers, not a ServiceStub proxy (ADR-0045 decision 3): IActivityClientController names PersistableBundle,
 * PictureInPictureParams, RemoteAnimationDefinition and SizeConfigurationBuckets, which framework.jar lacks, and ART
 * resolves every type in a proxy's signatures. Its other 63 methods return AIDL's defaults without decision 3's
 * once-per-method log line; Android's server refuses by permission only on hidden or system API paths
 * (registerRemoteAnimations, setForceSendResultForMediaProjection, setActivityRecordInputSinkEnabled,
 * restartActivityProcessIfVisible, invalidateHomeTaskSnapshot, dismissKeyguard with a message), so no permission is
 * checked here. The answers:
 *   - one task for the app, whose root is its first activity that is not finishing, as the launcher starts it in a
 *     task of its own and the app's own starts join it (step b3.5b2-1);
 *   - the requested orientation per activity, from its manifest until the activity sets one; ADR-0032's sessions turn
 *     with it later;
 *   - no caller: a launch from the home screen shares no identity, so the uid is Process.INVALID_UID where AIDL's
 *     default 0 would be root's, and a caller's content URI permission is denied;
 *   - a finish goes to ActivityThread's task (step b3.5b2-1): the last activity's asks the host's session, which
 *     answers with the quit (ADR-0043 step c1); without a session the last one's is refused, as AIDL's default
 *     answers;
 *   - results (step b3.5b2-2a): a finish's result goes to the activity that waits for it, which getCallingActivity
 *     and getCallingPackage name; finishActivity(requestCode) and finishAffinity finish the activities they name,
 *     and finishAndRemoveTask from the task's root ends the task;
 *   - up navigation (step b3.5b2-2b): navigateUpTo finishes up to the destination in the task and starts it there,
 *     and shouldUpRecreateTask compares the destination's affinity with the task's.
 */
package android.app;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.IBinder;
import android.os.Process;

final class LocalActivityClientController extends IActivityClientController.Default {
    /** The app's one task. Android numbers tasks from 1 since boot; Minato's first is the app's. */
    static final int TASK_ID = 1;

    private final ActivityThread mThread;

    LocalActivityClientController(ActivityThread thread) {
        mThread = thread;
    }

    @Override
    public boolean finishActivity(IBinder token, int resultCode, Intent resultData, int finishTask) {
        return mThread.finishActivity(token, resultCode, resultData, finishTask);
    }

    @Override
    public void finishSubActivity(IBinder token, String resultWho, int requestCode) {
        mThread.finishSubActivity(token, resultWho, requestCode);
    }

    @Override
    public boolean finishActivityAffinity(IBinder token) {
        return mThread.finishActivityAffinity(token);
    }

    @Override
    public ComponentName getCallingActivity(IBinder token) {
        return mThread.callingActivity(token);
    }

    @Override
    public String getCallingPackage(IBinder token) {
        return mThread.callingPackage(token);
    }

    @Override
    public boolean navigateUpTo(IBinder token, Intent target, String resolvedType, int resultCode, Intent resultData) {
        return mThread.navigateUpTo(token, target, resultCode, resultData);
    }

    @Override
    public boolean shouldUpRecreateTask(IBinder token, String destAffinity) {
        return mThread.shouldUpRecreateTask(token, destAffinity);
    }

    @Override
    public int getTaskForActivity(IBinder token, boolean onlyRoot) {
        ActivityThread.ActivityClientRecord r = mThread.mActivities.get(token);
        if (r == null || (onlyRoot && !mThread.isTaskRoot(token))) {
            return -1;
        }
        return TASK_ID;
    }

    @Override
    public boolean isTopOfTask(IBinder token) {
        return token != null && mThread.isTopOfTask(token);
    }

    @Override
    public void setRequestedOrientation(IBinder token, int requestedOrientation) {
        ActivityThread.ActivityClientRecord r = mThread.mActivities.get(token);
        if (r != null) {
            r.requestedOrientation = requestedOrientation;
        }
    }

    @Override
    public int getRequestedOrientation(IBinder token) {
        ActivityThread.ActivityClientRecord r = mThread.mActivities.get(token);
        return r != null ? r.requestedOrientation : android.content.pm.ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
    }

    @Override
    public int getLaunchedFromUid(IBinder token) {
        return Process.INVALID_UID;
    }

    @Override
    public int getActivityCallerUid(IBinder activityToken, IBinder callerToken) {
        return Process.INVALID_UID;
    }

    @Override
    public int checkActivityCallerContentUriPermission(IBinder activityToken, IBinder callerToken, Uri uri,
            int modeFlags, int userId) {
        return PackageManager.PERMISSION_DENIED;
    }
}
