// The written part of MediaProjectionManager (services batch B): the intent that asks the user to allow a screen
// capture, as AOSP builds it: the system UI's permission dialog [verified: services-b-test, not null]. Minato has no
// such dialog, so an app that starts it gets no result; a projection is not offered.
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;

class MediaProjectionManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public MediaProjectionManager(Context context) {
        mContext = context;
    }

    public Intent createScreenCaptureIntent() {
        final Intent intent = new Intent();
        intent.setComponent(new ComponentName("com.android.systemui",
                "com.android.systemui.media.MediaProjectionPermissionActivity"));
        return intent;
    }
}
