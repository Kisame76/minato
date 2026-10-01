/*
 * Interim (ADR-0039 step a): android.app.Application with its public no-argument constructor, so that an app's
 * Application subclass loads and initializes, and the two getters that b3.1's AOSP files call on the current
 * application (Build, StringBlock), and ActivityLifecycleCallbacks, which the public API's Activity names. Android
 * 15's extends ContextWrapper; AOSP's class replaces this one in step c, when the driver creates and attaches it.
 */
package android.app;

import android.content.res.Resources;
import android.os.Bundle;

public class Application {
    public Application() {
    }

    /** The app's package, as ActivityThread knows it. */
    public String getPackageName() {
        return ActivityThread.currentPackageName();
    }

    /** The app's Resources, which the driver gives it from b3.4 on; null until then. */
    public Resources getResources() {
        return null;
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
