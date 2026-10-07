/*
 * The activity task manager service of Minato's one app process (ADR-0039 step b3.5b2-1; ADR-0045 decisions 2 and 3):
 * what system_server's ActivityTaskManagerService answers an app's starts with, answered in the process, as
 * ServiceManager's "activity_task". A start goes to ActivityThread, which checks its explicit component against the
 * manifest at once, so that Instrumentation's checkStartActivityResult throws Android's ActivityNotFoundException, and
 * launches it on the main thread in the app's one task, with the activity that waits for its result (step
 * b3.5b2-2a). Not yet written, each answered with START_CANCELED and a line, so that the start throws rather than
 * doing nothing: several intents at once and PendingIntent's activities. No other app runs on Minato, so a start as
 * another caller is refused, as Android refuses one without the signature permission START_ACTIVITY_AS_CALLER, and
 * there is one user, the app's.
 */
package android.app;

import android.content.IIntentSender;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;

public final class LocalActivityTaskManager extends IActivityTaskManager.Stub {
    private static final String TAG = "ActivityTaskManager";

    private LocalActivityTaskManager() {
    }

    /** ServiceManager's maker for "activity_task": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalActivityTaskManager();
    }

    @Override
    public int startActivity(IApplicationThread caller, String callingPackage, String callingFeatureId, Intent intent,
            String resolvedType, IBinder resultTo, String resultWho, int requestCode, int flags,
            ProfilerInfo profilerInfo, Bundle options) {
        ActivityThread thread = ActivityThread.currentActivityThread();
        return thread != null ? thread.startActivity(intent, resultTo, resultWho, requestCode)
                : ActivityManager.START_CANCELED;
    }

    @Override
    public int startActivityAsUser(IApplicationThread caller, String callingPackage, String callingFeatureId,
            Intent intent, String resolvedType, IBinder resultTo, String resultWho, int requestCode, int flags,
            ProfilerInfo profilerInfo, Bundle options, int userId) {
        return startActivity(caller, callingPackage, callingFeatureId, intent, resolvedType, resultTo, resultWho,
                requestCode, flags, profilerInfo, options);
    }

    @Override
    public int startActivityAsCaller(IApplicationThread caller, String callingPackage, Intent intent,
            String resolvedType, IBinder resultTo, String resultWho, int requestCode, int flags,
            ProfilerInfo profilerInfo, Bundle options, boolean ignoreTargetSecurity, int userId) {
        return ActivityManager.START_PERMISSION_DENIED;
    }

    @Override
    public int startActivities(IApplicationThread caller, String callingPackage, String callingFeatureId,
            Intent[] intents, String[] resolvedTypes, IBinder resultTo, Bundle options, int userId) {
        return unwritten("startActivities", "several intents at once");
    }

    @Override
    public int startActivityIntentSender(IApplicationThread caller, IIntentSender target, IBinder whitelistToken,
            Intent fillInIntent, String resolvedType, IBinder resultTo, String resultWho, int requestCode,
            int flagsMask, int flagsValues, Bundle options) {
        return unwritten("startActivityIntentSender", "a PendingIntent's activity");
    }

    /** As Android's when no other activity matches, as no implicit intent resolves yet (step b3.5b2-3). */
    @Override
    public boolean startNextMatchingActivity(IBinder callingActivity, Intent intent, Bundle options) {
        return false;
    }

    /** As Android's without a voice interaction service, which Minato does not have. */
    @Override
    public boolean supportsLocalVoiceInteraction() {
        return false;
    }

    /** The process's own controller, which ActivityThread hands ActivityClient at the bind (step c2c3). */
    @Override
    public IActivityClientController getActivityClientController() {
        ActivityThread thread = ActivityThread.currentActivityThread();
        return thread != null ? thread.mActivityClientController : null;
    }

    private static int unwritten(String method, String what) {
        Log.w(TAG, method + ": " + what + " comes with a later step (ADR-0039 b3.5b2)");
        return ActivityManager.START_CANCELED;
    }
}
