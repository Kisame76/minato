// The written part of ActivityOptions (the Unity test APK retest 10: Activity's methods name the type, and a class whose method
// types cannot be resolved cannot be read by reflection; services batch E: its data), which tools/managergen joins
// with the API's methods. AOSP's class is 2,800 lines, most of them the system's window transitions (remote animations
// and transitions, window container tokens, shared element coordinators), which Minato's single activity window does
// not run. Ours is the options object of AOSP's: every factory and setter of the API keeps what it is given, update
// copies as AOSP's does, toBundle writes AOSP's keys and values and the Bundle constructor reads them back, so options
// that travel through startActivity(intent, bundle) and PendingIntent.send come back the same [verified:
// activity-options-test, its bundles on Android 15]. What does not run: the animations themselves (custom, scale up,
// clip reveal, thumbnail, task launch behind are kept and never played), the listeners of animation start, finish and
// abort (always null, as an app that gives none leaves them), the scene transition (makeSceneTransitionAnimation
// answers as AOSP's does for a window without activity transitions: the default animation, no shared element checks),
// and the hidden members that name the system's window containers, remote animations and task organizer; those stay
// the generated ones that log and return a default.
// Includes the hidden members the taken files call: Activity's and ActivityTransitionState's scene transition info,
// PendingIntent's caller display and RemoteViews' pending intent launch, and IntentSender's background start mode,
// which its class initialisation sets on options of makeBasic and takes as a Bundle (ComponentOptions' key and rule),
// so that a class that holds an IntentSender, such as ClipData's items, loads (ADR-0057 step a).
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.ResultReceiver;
import android.util.Pair;
import android.view.View;
import java.util.ArrayList;

class ActivityOptions {
    /** @hide */
    public static final int MODE_BACKGROUND_ACTIVITY_START_COMPAT = -1;

    /** @hide */
    public static final int ANIM_UNDEFINED = -1;
    /** @hide */
    public static final int ANIM_NONE = 0;
    /** @hide */
    public static final int ANIM_CUSTOM = 1;
    /** @hide */
    public static final int ANIM_SCALE_UP = 2;
    /** @hide */
    public static final int ANIM_THUMBNAIL_SCALE_UP = 3;
    /** @hide */
    public static final int ANIM_THUMBNAIL_SCALE_DOWN = 4;
    /** @hide */
    public static final int ANIM_SCENE_TRANSITION = 5;
    /** @hide */
    public static final int ANIM_DEFAULT = 6;
    /** @hide */
    public static final int ANIM_LAUNCH_TASK_BEHIND = 7;
    /** @hide */
    public static final int ANIM_THUMBNAIL_ASPECT_SCALE_UP = 8;
    /** @hide */
    public static final int ANIM_THUMBNAIL_ASPECT_SCALE_DOWN = 9;
    /** @hide */
    public static final int ANIM_CUSTOM_IN_PLACE = 10;
    /** @hide */
    public static final int ANIM_CLIP_REVEAL = 11;

    /** @hide */
    public static final String KEY_PACKAGE_NAME = "android:activity.packageName";
    /** @hide */
    public static final String KEY_LAUNCH_BOUNDS = "android:activity.launchBounds";
    /** @hide */
    public static final String KEY_ANIM_TYPE = "android:activity.animType";
    /** @hide */
    public static final String KEY_ANIM_ENTER_RES_ID = "android:activity.animEnterRes";
    /** @hide */
    public static final String KEY_ANIM_EXIT_RES_ID = "android:activity.animExitRes";
    /** @hide */
    public static final String KEY_ANIM_IN_PLACE_RES_ID = "android:activity.animInPlaceRes";
    /** @hide */
    public static final String KEY_ANIM_BACKGROUND_COLOR = "android:activity.backgroundColor";
    /** @hide */
    public static final String KEY_ANIM_THUMBNAIL = "android:activity.animThumbnail";
    /** @hide */
    public static final String KEY_ANIM_START_X = "android:activity.animStartX";
    /** @hide */
    public static final String KEY_ANIM_START_Y = "android:activity.animStartY";
    /** @hide */
    public static final String KEY_ANIM_WIDTH = "android:activity.animWidth";
    /** @hide */
    public static final String KEY_ANIM_HEIGHT = "android:activity.animHeight";
    /** @hide */
    public static final String KEY_ANIM_START_LISTENER = "android:activity.animStartListener";
    private static final String KEY_ANIM_ABORT_LISTENER = "android:activity.animAbortListener";
    private static final String KEY_LOCK_TASK_MODE = "android:activity.lockTaskMode";
    private static final String KEY_SHARE_IDENTITY = "android:activity.shareIdentity";
    private static final String KEY_LAUNCH_DISPLAY_ID = "android.activity.launchDisplayId";
    private static final String KEY_CALLER_DISPLAY_ID = "android.activity.callerDisplayId";
    private static final String KEY_PENDING_INTENT_LAUNCH_FLAGS = "android.activity.pendingIntentLaunchFlags";
    private static final String KEY_USAGE_TIME_REPORT = "android:activity.usageTimeReport";
    private static final String KEY_INSTANT_APP_VERIFICATION_BUNDLE = "android:instantapps.installerbundle";
    private static final String KEY_SPLASH_SCREEN_STYLE = "android.activity.splashScreenStyle";
    private static final String KEY_LAUNCH_INTO_PIP_PARAMS = "android.activity.launchIntoPipParams";
    private static final String KEY_PENDING_INTENT_CREATOR_BACKGROUND_ACTIVITY_START_MODE =
            "android.activity.pendingIntentCreatorBackgroundActivityStartMode";
    /** ComponentOptions' keys. */
    private static final String KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED =
            "android.pendingIntent.backgroundActivityAllowed";
    private static final String KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED_BY_PERMISSION =
            "android.pendingIntent.backgroundActivityAllowedByPermission";

    private String mPackageName;
    private Rect mLaunchBounds;
    private int mAnimationType = ANIM_UNDEFINED;
    private int mCustomEnterResId;
    private int mCustomExitResId;
    private int mCustomInPlaceResId;
    private int mCustomBackgroundColor;
    private Bitmap mThumbnail;
    private int mStartX;
    private int mStartY;
    private int mWidth;
    private int mHeight;
    private PendingIntent mUsageTimeReport;
    private int mLaunchDisplayId = -1;
    private int mCallerDisplayId = -1;
    private int mPendingIntentLaunchFlags;
    private boolean mLockTaskMode;
    private boolean mShareIdentity;
    private int mSplashScreenStyle = -1;
    private Bundle mAppVerificationBundle;
    private android.app.PictureInPictureParams mLaunchIntoPipParams;
    private int mPendingIntentCreatorBackgroundActivityStartMode;
    private int mPendingIntentBackgroundActivityStartMode;
    private boolean mPendingIntentBackgroundActivityLaunchAllowedByPermission;

    private ActivityOptions() {
    }

    /** @hide As AOSP's: the options another process made, read from their bundle. */
    public ActivityOptions(Bundle opts) {
        mPendingIntentBackgroundActivityStartMode = opts.getInt(KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED, 0);
        mPendingIntentBackgroundActivityLaunchAllowedByPermission =
                opts.getBoolean(KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED_BY_PERMISSION, false);
        mPackageName = opts.getString(KEY_PACKAGE_NAME);
        try {
            mUsageTimeReport = opts.getParcelable(KEY_USAGE_TIME_REPORT, PendingIntent.class);
        } catch (RuntimeException e) {
            android.util.Log.w("ActivityOptions", e);
        }
        mLaunchBounds = opts.getParcelable(KEY_LAUNCH_BOUNDS, Rect.class);
        mAnimationType = opts.getInt(KEY_ANIM_TYPE, ANIM_UNDEFINED);
        switch (mAnimationType) {
            case ANIM_CUSTOM:
                mCustomEnterResId = opts.getInt(KEY_ANIM_ENTER_RES_ID, 0);
                mCustomExitResId = opts.getInt(KEY_ANIM_EXIT_RES_ID, 0);
                mCustomBackgroundColor = opts.getInt(KEY_ANIM_BACKGROUND_COLOR, 0);
                break;
            case ANIM_CUSTOM_IN_PLACE:
                mCustomInPlaceResId = opts.getInt(KEY_ANIM_IN_PLACE_RES_ID, 0);
                break;
            case ANIM_SCALE_UP:
            case ANIM_CLIP_REVEAL:
                mStartX = opts.getInt(KEY_ANIM_START_X, 0);
                mStartY = opts.getInt(KEY_ANIM_START_Y, 0);
                mWidth = opts.getInt(KEY_ANIM_WIDTH, 0);
                mHeight = opts.getInt(KEY_ANIM_HEIGHT, 0);
                break;
            case ANIM_THUMBNAIL_SCALE_UP:
            case ANIM_THUMBNAIL_SCALE_DOWN:
            case ANIM_THUMBNAIL_ASPECT_SCALE_UP:
            case ANIM_THUMBNAIL_ASPECT_SCALE_DOWN:
                final android.hardware.HardwareBuffer buffer =
                        opts.getParcelable(KEY_ANIM_THUMBNAIL, android.hardware.HardwareBuffer.class);
                if (buffer != null) {
                    mThumbnail = Bitmap.wrapHardwareBuffer(buffer, null);
                }
                mStartX = opts.getInt(KEY_ANIM_START_X, 0);
                mStartY = opts.getInt(KEY_ANIM_START_Y, 0);
                mWidth = opts.getInt(KEY_ANIM_WIDTH, 0);
                mHeight = opts.getInt(KEY_ANIM_HEIGHT, 0);
                break;
        }
        mLockTaskMode = opts.getBoolean(KEY_LOCK_TASK_MODE, false);
        mShareIdentity = opts.getBoolean(KEY_SHARE_IDENTITY, false);
        mLaunchDisplayId = opts.getInt(KEY_LAUNCH_DISPLAY_ID, -1);
        mCallerDisplayId = opts.getInt(KEY_CALLER_DISPLAY_ID, -1);
        mPendingIntentLaunchFlags = opts.getInt(KEY_PENDING_INTENT_LAUNCH_FLAGS, 0);
        mAppVerificationBundle = opts.getBundle(KEY_INSTANT_APP_VERIFICATION_BUNDLE);
        mSplashScreenStyle = opts.getInt(KEY_SPLASH_SCREEN_STYLE);
        mLaunchIntoPipParams = opts.getParcelable(KEY_LAUNCH_INTO_PIP_PARAMS, android.app.PictureInPictureParams.class);
        mPendingIntentCreatorBackgroundActivityStartMode =
                opts.getInt(KEY_PENDING_INTENT_CREATOR_BACKGROUND_ACTIVITY_START_MODE, 0);
    }

    /** @hide */
    public static ActivityOptions fromBundle(Bundle bOptions) {
        return bOptions != null ? new ActivityOptions(bOptions) : null;
    }

    public static ActivityOptions makeBasic() {
        return new ActivityOptions();
    }

    public static ActivityOptions makeCustomAnimation(Context context, int enterResId, int exitResId) {
        return makeCustomAnimation(context, enterResId, exitResId, 0);
    }

    public static ActivityOptions makeCustomAnimation(Context context, int enterResId, int exitResId,
            int backgroundColor) {
        final ActivityOptions opts = new ActivityOptions();
        opts.mPackageName = context.getPackageName();
        opts.mAnimationType = ANIM_CUSTOM;
        opts.mCustomEnterResId = enterResId;
        opts.mCustomExitResId = exitResId;
        opts.mCustomBackgroundColor = backgroundColor;
        return opts;
    }

    public static ActivityOptions makeScaleUpAnimation(View source, int startX, int startY, int width, int height) {
        final ActivityOptions opts = new ActivityOptions();
        opts.mPackageName = source.getContext().getPackageName();
        opts.mAnimationType = ANIM_SCALE_UP;
        final int[] pts = new int[2];
        source.getLocationOnScreen(pts);
        opts.mStartX = pts[0] + startX;
        opts.mStartY = pts[1] + startY;
        opts.mWidth = width;
        opts.mHeight = height;
        return opts;
    }

    public static ActivityOptions makeClipRevealAnimation(View source, int startX, int startY, int width,
            int height) {
        final ActivityOptions opts = new ActivityOptions();
        opts.mAnimationType = ANIM_CLIP_REVEAL;
        final int[] pts = new int[2];
        source.getLocationOnScreen(pts);
        opts.mStartX = pts[0] + startX;
        opts.mStartY = pts[1] + startY;
        opts.mWidth = width;
        opts.mHeight = height;
        return opts;
    }

    public static ActivityOptions makeThumbnailScaleUpAnimation(View source, Bitmap thumbnail, int startX,
            int startY) {
        final ActivityOptions opts = new ActivityOptions();
        opts.mPackageName = source.getContext().getPackageName();
        opts.mAnimationType = ANIM_THUMBNAIL_SCALE_UP;
        opts.mThumbnail = thumbnail;
        final int[] pts = new int[2];
        source.getLocationOnScreen(pts);
        opts.mStartX = pts[0] + startX;
        opts.mStartY = pts[1] + startY;
        return opts;
    }

    public static ActivityOptions makeTaskLaunchBehind() {
        final ActivityOptions opts = new ActivityOptions();
        opts.mAnimationType = ANIM_LAUNCH_TASK_BEHIND;
        return opts;
    }

    public static ActivityOptions makeLaunchIntoPip(android.app.PictureInPictureParams pictureInPictureParams) {
        final ActivityOptions opts = new ActivityOptions();
        final android.app.PictureInPictureParams.Builder builder =
                new android.app.PictureInPictureParams.Builder(pictureInPictureParams);
        try {
            // The package-private flag that makes the params a launch into PiP, set by name as the compile-only stub
            // of the managers check does not have it.
            final java.lang.reflect.Method flag = android.app.PictureInPictureParams.Builder.class
                    .getDeclaredMethod("setIsLaunchIntoPip", boolean.class);
            flag.setAccessible(true);
            flag.invoke(builder, true);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
        opts.mLaunchIntoPipParams = builder.build();
        return opts;
    }

    /**
     * No window transition runs, so, as AOSP's for a window without activity transitions, the options ask for the
     * default animation and the shared elements are not looked at.
     */
    public static ActivityOptions makeSceneTransitionAnimation(android.app.Activity activity, View sharedElement,
            String sharedElementName) {
        return makeSceneTransitionAnimation(activity);
    }

    @SafeVarargs
    public static ActivityOptions makeSceneTransitionAnimation(android.app.Activity activity,
            Pair<View, String>... sharedElements) {
        final ActivityOptions opts = new ActivityOptions();
        opts.mAnimationType = ANIM_DEFAULT;
        return opts;
    }

    public ActivityOptions setLaunchBounds(Rect screenSpacePixelRect) {
        mLaunchBounds = screenSpacePixelRect != null ? new Rect(screenSpacePixelRect) : null;
        return this;
    }

    public Rect getLaunchBounds() {
        return mLaunchBounds;
    }

    public ActivityOptions setLaunchDisplayId(int launchDisplayId) {
        mLaunchDisplayId = launchDisplayId;
        return this;
    }

    public int getLaunchDisplayId() {
        return mLaunchDisplayId;
    }

    public ActivityOptions setLockTaskEnabled(boolean lockTaskMode) {
        mLockTaskMode = lockTaskMode;
        return this;
    }

    public boolean getLockTaskMode() {
        return mLockTaskMode;
    }

    public ActivityOptions setShareIdentityEnabled(boolean shareIdentity) {
        mShareIdentity = shareIdentity;
        return this;
    }

    public boolean isShareIdentityEnabled() {
        return mShareIdentity;
    }

    public ActivityOptions setSplashScreenStyle(int style) {
        if (style == 1 || style == 2) {
            mSplashScreenStyle = style;
        }
        return this;
    }

    public int getSplashScreenStyle() {
        return mSplashScreenStyle;
    }

    public ActivityOptions setAppVerificationBundle(Bundle bundle) {
        mAppVerificationBundle = bundle;
        return this;
    }

    public void requestUsageTimeReport(PendingIntent receiver) {
        mUsageTimeReport = receiver;
    }

    /** As ComponentOptions': the known modes are kept, any other value is taken as a variant of allowing. */
    public ActivityOptions setPendingIntentBackgroundActivityStartMode(int state) {
        mPendingIntentBackgroundActivityStartMode = componentMode(state);
        return this;
    }

    public int getPendingIntentBackgroundActivityStartMode() {
        return mPendingIntentBackgroundActivityStartMode;
    }

    public ActivityOptions setPendingIntentCreatorBackgroundActivityStartMode(int mode) {
        mPendingIntentCreatorBackgroundActivityStartMode = mode;
        return this;
    }

    public int getPendingIntentCreatorBackgroundActivityStartMode() {
        return mPendingIntentCreatorBackgroundActivityStartMode;
    }

    @Deprecated
    public void setPendingIntentBackgroundActivityLaunchAllowed(boolean allowed) {
        mPendingIntentBackgroundActivityStartMode = allowed ? MODE_BACKGROUND_ACTIVITY_START_ALLOWED
                : MODE_BACKGROUND_ACTIVITY_START_DENIED;
    }

    @Deprecated
    public boolean isPendingIntentBackgroundActivityLaunchAllowed() {
        return mPendingIntentBackgroundActivityStartMode != MODE_BACKGROUND_ACTIVITY_START_DENIED;
    }

    private static int componentMode(int state) {
        switch (state) {
            case MODE_BACKGROUND_ACTIVITY_START_SYSTEM_DEFINED:
            case MODE_BACKGROUND_ACTIVITY_START_DENIED:
            case MODE_BACKGROUND_ACTIVITY_START_COMPAT:
            case MODE_BACKGROUND_ACTIVITY_START_ALLOWED:
                return state;
            default:
                return MODE_BACKGROUND_ACTIVITY_START_ALLOWED;
        }
    }

    /** @hide */
    public String getPackageName() {
        return mPackageName;
    }

    /** @hide */
    public int getAnimationType() {
        return mAnimationType;
    }

    /** @hide */
    public int getCustomEnterResId() {
        return mCustomEnterResId;
    }

    /** @hide */
    public int getCustomExitResId() {
        return mCustomExitResId;
    }

    /** @hide */
    public int getCustomBackgroundColor() {
        return mCustomBackgroundColor;
    }

    /** @hide */
    public int getStartX() {
        return mStartX;
    }

    /** @hide */
    public int getStartY() {
        return mStartY;
    }

    /** @hide */
    public int getWidth() {
        return mWidth;
    }

    /** @hide */
    public int getHeight() {
        return mHeight;
    }

    /** @hide */
    public boolean getLaunchTaskBehind() {
        return mAnimationType == ANIM_LAUNCH_TASK_BEHIND;
    }

    /** @hide */
    public int getCallerDisplayId() {
        return mCallerDisplayId;
    }

    /** @hide */
    public PendingIntent getUsageTimeReport() {
        return mUsageTimeReport;
    }

    /** @hide */
    public Bundle getAppVerificationBundle() {
        return mAppVerificationBundle;
    }

    /** @hide */
    public void setPendingIntentLaunchFlags(int flags) {
        mPendingIntentLaunchFlags = flags;
    }

    /** @hide */
    public int getPendingIntentLaunchFlags() {
        return mPendingIntentLaunchFlags;
    }

    /** @hide */
    public ActivityOptions setCallerDisplayId(int callerDisplayId) {
        mCallerDisplayId = callerDisplayId;
        return this;
    }

    /** @hide */
    public void setPendingIntentBackgroundActivityLaunchAllowedByPermission(boolean allowed) {
        mPendingIntentBackgroundActivityLaunchAllowedByPermission = allowed;
    }

    /** @hide */
    public boolean isPendingIntentBackgroundActivityLaunchAllowedByPermission() {
        return mPendingIntentBackgroundActivityLaunchAllowedByPermission;
    }

    /** As AOSP's: what of the other options the animation type carries replaces this one's. */
    public void update(ActivityOptions otherOptions) {
        if (otherOptions.mPackageName != null) {
            mPackageName = otherOptions.mPackageName;
        }
        mUsageTimeReport = otherOptions.mUsageTimeReport;
        mAnimationType = otherOptions.mAnimationType;
        switch (otherOptions.mAnimationType) {
            case ANIM_CUSTOM:
                mCustomEnterResId = otherOptions.mCustomEnterResId;
                mCustomExitResId = otherOptions.mCustomExitResId;
                mCustomBackgroundColor = otherOptions.mCustomBackgroundColor;
                mThumbnail = null;
                break;
            case ANIM_CUSTOM_IN_PLACE:
                mCustomInPlaceResId = otherOptions.mCustomInPlaceResId;
                break;
            case ANIM_SCALE_UP:
                mStartX = otherOptions.mStartX;
                mStartY = otherOptions.mStartY;
                mWidth = otherOptions.mWidth;
                mHeight = otherOptions.mHeight;
                break;
            case ANIM_THUMBNAIL_SCALE_UP:
            case ANIM_THUMBNAIL_SCALE_DOWN:
            case ANIM_THUMBNAIL_ASPECT_SCALE_UP:
            case ANIM_THUMBNAIL_ASPECT_SCALE_DOWN:
                mThumbnail = otherOptions.mThumbnail;
                mStartX = otherOptions.mStartX;
                mStartY = otherOptions.mStartY;
                mWidth = otherOptions.mWidth;
                mHeight = otherOptions.mHeight;
                break;
            case ANIM_SCENE_TRANSITION:
                mThumbnail = null;
                break;
        }
        mLockTaskMode = otherOptions.mLockTaskMode;
        mShareIdentity = otherOptions.mShareIdentity;
        mLaunchIntoPipParams = otherOptions.mLaunchIntoPipParams;
    }

    /** As AOSP's ComponentOptions.toBundle and ActivityOptions.toBundle: what differs from the defaults. */
    public Bundle toBundle() {
        final Bundle b = new Bundle();
        if (mPendingIntentBackgroundActivityStartMode != 0) {
            b.putInt(KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED, mPendingIntentBackgroundActivityStartMode);
        }
        if (mPendingIntentBackgroundActivityLaunchAllowedByPermission) {
            b.putBoolean(KEY_PENDING_INTENT_BACKGROUND_ACTIVITY_ALLOWED_BY_PERMISSION, true);
        }
        if (mPackageName != null) {
            b.putString(KEY_PACKAGE_NAME, mPackageName);
        }
        if (mLaunchBounds != null) {
            b.putParcelable(KEY_LAUNCH_BOUNDS, mLaunchBounds);
        }
        if (mAnimationType != ANIM_UNDEFINED) {
            b.putInt(KEY_ANIM_TYPE, mAnimationType);
        }
        if (mUsageTimeReport != null) {
            b.putParcelable(KEY_USAGE_TIME_REPORT, mUsageTimeReport);
        }
        switch (mAnimationType) {
            case ANIM_CUSTOM:
                b.putInt(KEY_ANIM_ENTER_RES_ID, mCustomEnterResId);
                b.putInt(KEY_ANIM_EXIT_RES_ID, mCustomExitResId);
                b.putInt(KEY_ANIM_BACKGROUND_COLOR, mCustomBackgroundColor);
                b.putBinder(KEY_ANIM_START_LISTENER, null);
                break;
            case ANIM_CUSTOM_IN_PLACE:
                b.putInt(KEY_ANIM_IN_PLACE_RES_ID, mCustomInPlaceResId);
                break;
            case ANIM_SCALE_UP:
            case ANIM_CLIP_REVEAL:
                b.putInt(KEY_ANIM_START_X, mStartX);
                b.putInt(KEY_ANIM_START_Y, mStartY);
                b.putInt(KEY_ANIM_WIDTH, mWidth);
                b.putInt(KEY_ANIM_HEIGHT, mHeight);
                break;
            case ANIM_THUMBNAIL_SCALE_UP:
            case ANIM_THUMBNAIL_SCALE_DOWN:
            case ANIM_THUMBNAIL_ASPECT_SCALE_UP:
            case ANIM_THUMBNAIL_ASPECT_SCALE_DOWN:
                if (mThumbnail != null) {
                    final Bitmap hwBitmap = mThumbnail.copy(Bitmap.Config.HARDWARE, false);
                    if (hwBitmap != null) {
                        b.putParcelable(KEY_ANIM_THUMBNAIL, hwBitmap.getHardwareBuffer());
                    }
                }
                b.putInt(KEY_ANIM_START_X, mStartX);
                b.putInt(KEY_ANIM_START_Y, mStartY);
                b.putInt(KEY_ANIM_WIDTH, mWidth);
                b.putInt(KEY_ANIM_HEIGHT, mHeight);
                b.putBinder(KEY_ANIM_START_LISTENER, null);
                break;
        }
        if (mLockTaskMode) {
            b.putBoolean(KEY_LOCK_TASK_MODE, true);
        }
        if (mShareIdentity) {
            b.putBoolean(KEY_SHARE_IDENTITY, true);
        }
        if (mLaunchDisplayId != -1) {
            b.putInt(KEY_LAUNCH_DISPLAY_ID, mLaunchDisplayId);
        }
        if (mCallerDisplayId != -1) {
            b.putInt(KEY_CALLER_DISPLAY_ID, mCallerDisplayId);
        }
        if (mPendingIntentLaunchFlags != 0) {
            b.putInt(KEY_PENDING_INTENT_LAUNCH_FLAGS, mPendingIntentLaunchFlags);
        }
        if (mAppVerificationBundle != null) {
            b.putBundle(KEY_INSTANT_APP_VERIFICATION_BUNDLE, mAppVerificationBundle);
        }
        if (mSplashScreenStyle != 0) {
            b.putInt(KEY_SPLASH_SCREEN_STYLE, mSplashScreenStyle);
        }
        if (mLaunchIntoPipParams != null) {
            b.putParcelable(KEY_LAUNCH_INTO_PIP_PARAMS, mLaunchIntoPipParams);
        }
        if (mPendingIntentCreatorBackgroundActivityStartMode != 0) {
            b.putInt(KEY_PENDING_INTENT_CREATOR_BACKGROUND_ACTIVITY_START_MODE,
                    mPendingIntentCreatorBackgroundActivityStartMode);
        }
        b.putBinder(KEY_ANIM_ABORT_LISTENER, null);
        return b;
    }

    /** @hide */
    @Override
    public String toString() {
        return "ActivityOptions(" + hashCode() + "), mPackageName=" + mPackageName + ", mAnimationType="
                + mAnimationType + ", mStartX=" + mStartX + ", mStartY=" + mStartY + ", mWidth=" + mWidth
                + ", mHeight=" + mHeight + ", mLaunchDisplayId=" + mLaunchDisplayId;
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

}
