/*
 * Interim (ADR-0039 step a): android.app.Application with its public no-argument constructor, onCreate, which apps
 * override (styled-text-test, b3.4b), and ActivityLifecycleCallbacks, which the public API's Activity names. Since
 * step c1 it extends AOSP's ContextWrapper, as Android 15's does, and LoadedApk.makeApplication attaches the app's
 * ContextImpl to it before ActivityThread.currentApplication() gives it out; until then, as on Android, it has no
 * base. AOSP's class replaces this one with step c2's Activity, which calls its lifecycle dispatchers.
 */
package android.app;

import android.content.Context;
import android.content.ContextWrapper;
import android.os.Bundle;

public class Application extends ContextWrapper {
    /**
     * The app's package as its ContextImpl knows it, which AOSP's Application keeps from attach on.
     *
     * @hide
     */
    public LoadedApk mLoadedApk;

    public Application() {
        super(null);
    }

    /**
     * As AOSP's: the app's ContextImpl becomes its base, and its LoadedApk the Application's.
     *
     * @hide
     */
    final void attach(Context context) {
        attachBaseContext(context);
        mLoadedApk = ContextImpl.getImpl(context).mPackageInfo;
    }

    /** Called as the app starts, before its components; the driver calls it from step c2 on. */
    public void onCreate() {
    }

    /** Android 15's lifecycle callbacks, with the tag's methods; AOSP's Application in step c calls them. */
    public interface ActivityLifecycleCallbacks {
        default void onActivityPreCreated(Activity activity, Bundle savedInstanceState) {
        }

        void onActivityCreated(Activity activity, Bundle savedInstanceState);

        default void onActivityPostCreated(Activity activity, Bundle savedInstanceState) {
        }

        default void onActivityPreStarted(Activity activity) {
        }

        void onActivityStarted(Activity activity);

        default void onActivityPostStarted(Activity activity) {
        }

        default void onActivityPreResumed(Activity activity) {
        }

        void onActivityResumed(Activity activity);

        default void onActivityPostResumed(Activity activity) {
        }

        default void onActivityPrePaused(Activity activity) {
        }

        void onActivityPaused(Activity activity);

        default void onActivityPostPaused(Activity activity) {
        }

        default void onActivityPreStopped(Activity activity) {
        }

        void onActivityStopped(Activity activity);

        default void onActivityPostStopped(Activity activity) {
        }

        default void onActivityPreSaveInstanceState(Activity activity, Bundle outState) {
        }

        void onActivitySaveInstanceState(Activity activity, Bundle outState);

        default void onActivityPostSaveInstanceState(Activity activity, Bundle outState) {
        }

        default void onActivityPreDestroyed(Activity activity) {
        }

        void onActivityDestroyed(Activity activity);

        default void onActivityPostDestroyed(Activity activity) {
        }
    }
}
