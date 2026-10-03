/*
 * Interim (ADR-0039 b3.5a): android.app.AppDetailsActivity without its supertype, so that PackageManager's static
 * initializer, which takes APP_DETAILS_ACTIVITY_CLASS_NAME from AppDetailsActivity.class.getName(), loads before
 * Activity is in framework.jar. Android 15's extends Activity and forwards to the Settings app's details page; AOSP's
 * class replaces this one in step c, with Activity.
 */
package android.app;

/** @hide */
public class AppDetailsActivity {
}
