/*
 * The activity task manager (ADR-0039 step b3.5b2-1): our class under AOSP's name, with what framework.jar's code asks
 * of it: the service, from ServiceManager's "activity_task" as AOSP's getService takes it; whether the device supports
 * multi-window, which WindowManager.hasWindowExtensionsEnabled asks for androidx.window; and the limit of
 * picture-in-picture actions, which Activity.getMaxNumPictureInPictureActions gives apps. AOSP's class is a @TestApi
 * system service whose other methods ask the service about tasks, recents and lock task mode, which framework.jar does
 * not call and an app cannot reach (hidden API).
 */
package android.app;

import android.content.Context;
import android.os.IBinder;
import android.os.ServiceManager;
import android.util.Singleton;

public class ActivityTaskManager {
    /** The id of no task. */
    public static final int INVALID_TASK_ID = -1;

    private ActivityTaskManager() {
    }

    /** As AOSP's: the service of ServiceManager's "activity_task". */
    public static IActivityTaskManager getService() {
        return IActivityTaskManagerSingleton.get();
    }

    private static final Singleton<IActivityTaskManager> IActivityTaskManagerSingleton =
            new Singleton<IActivityTaskManager>() {
                @Override
                protected IActivityTaskManager create() {
                    final IBinder b = ServiceManager.getService(Context.ACTIVITY_TASK_SERVICE);
                    return IActivityTaskManager.Stub.asInterface(b);
                }
            };

    /**
     * Whether the device supports multi-window: no, as Minato shows one app's window at a time (ADR-0028, decision 6),
     * without split screen or freeform windows. Android 15 answers yes on a phone (config_supportsMultiWindow).
     */
    public static boolean supportsMultiWindow(Context context) {
        return false;
    }

    /** As AOSP's: the framework's config_pictureInPictureMaxNumberOfActions. */
    public static int getMaxNumPictureInPictureActions(Context context) {
        return context.getResources().getInteger(
                com.android.internal.R.integer.config_pictureInPictureMaxNumberOfActions);
    }
}
