// The hand file of our ActivityManager (ADR-0039 step c2c3, ADR-0045 decision 3): tools/managergen writes the class
// from the public API, whose methods log once and return their default, and the members here are the hidden ones
// that taken AOSP files call, with TaskDescription as AOSP's but its parcel and XML forms. Every themed activity
// keeps its task description in one (Activity.onApplyThemeResource), so TaskDescription holds what it is given.
// No Context gives an ActivityManager yet (ADR-0045's local services); its task description is all c2c3 needs.
// TaskDescription's members, the start codes and getLauncherLargeIconSizeInner follow ActivityManager.java at
// android-15.0.0_r1 (Apache-2.0).
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.drawable.Icon;
import android.util.DisplayMetrics;
import com.android.minato.services.Unwritten;

class ActivityManager {
    private static final int FIRST_START_FATAL_ERROR_CODE = -100;
    private static final int LAST_START_FATAL_ERROR_CODE = -1;
    private static final int FIRST_START_SUCCESS_CODE = 0;

    /** @hide */
    public static final int START_VOICE_HIDDEN_SESSION = FIRST_START_FATAL_ERROR_CODE;

    /** @hide */
    public static final int START_VOICE_NOT_ACTIVE_SESSION = FIRST_START_FATAL_ERROR_CODE + 1;

    /** @hide */
    public static final int START_NOT_VOICE_COMPATIBLE = FIRST_START_FATAL_ERROR_CODE + 3;

    /** @hide */
    public static final int START_CANCELED = FIRST_START_FATAL_ERROR_CODE + 4;

    /** @hide */
    public static final int START_NOT_ACTIVITY = FIRST_START_FATAL_ERROR_CODE + 5;

    /** @hide */
    public static final int START_PERMISSION_DENIED = FIRST_START_FATAL_ERROR_CODE + 6;

    /** @hide */
    public static final int START_FORWARD_AND_REQUEST_CONFLICT = FIRST_START_FATAL_ERROR_CODE + 7;

    /** @hide */
    public static final int START_CLASS_NOT_FOUND = FIRST_START_FATAL_ERROR_CODE + 8;

    /** @hide */
    public static final int START_INTENT_NOT_RESOLVED = FIRST_START_FATAL_ERROR_CODE + 9;

    /** @hide */
    public static final int START_ASSISTANT_HIDDEN_SESSION = FIRST_START_FATAL_ERROR_CODE + 10;

    /** @hide */
    public static final int START_ASSISTANT_NOT_ACTIVE_SESSION = FIRST_START_FATAL_ERROR_CODE + 11;

    /** @hide */
    public static final int START_SUCCESS = FIRST_START_SUCCESS_CODE;

    /** @hide */
    public static final int START_RETURN_INTENT_TO_CALLER = FIRST_START_SUCCESS_CODE + 1;

    /** @hide */
    public static final int START_FLAG_ONLY_IF_NEEDED = 1 << 0;

    /** @hide */
    public static final int INTENT_SENDER_ACTIVITY_RESULT = 3;

    /** @hide */
    public static final boolean isStartResultFatalError(int result) {
        return FIRST_START_FATAL_ERROR_CODE <= result && result <= LAST_START_FATAL_ERROR_CODE;
    }

    /** @hide The activity manager service, which Minato has none of (ADR-0045's local services). */
    public static IActivityManager getService() {
        Unwritten.log("android.app.ActivityManager.getService()", "null");
        return null;
    }

    /** @hide As outside system_server on Android: the system is ready by the time an app runs. */
    public static boolean isSystemReady() {
        return true;
    }

    /** @hide An iPhone is no low-RAM device (ro.config.low_ram), and Minato has no developer option to force it. */
    public static boolean isLowRamDeviceStatic() {
        return false;
    }

    /** @hide As AOSP's: the launcher's large icon size, which scales a task description's in-memory icon. */
    static int getLauncherLargeIconSizeInner(Context context) {
        final Resources res = context.getResources();
        final int size = res.getDimensionPixelSize(android.R.dimen.app_icon_size);
        final int sw = res.getConfiguration().smallestScreenWidthDp;
        if (sw < 600) {
            return size;
        }
        final int density = res.getDisplayMetrics().densityDpi;
        switch (density) {
            case DisplayMetrics.DENSITY_LOW:
                return (size * DisplayMetrics.DENSITY_MEDIUM) / DisplayMetrics.DENSITY_LOW;
            case DisplayMetrics.DENSITY_MEDIUM:
                return (size * DisplayMetrics.DENSITY_HIGH) / DisplayMetrics.DENSITY_MEDIUM;
            case DisplayMetrics.DENSITY_TV:
                return (size * DisplayMetrics.DENSITY_XHIGH) / DisplayMetrics.DENSITY_HIGH;
            case DisplayMetrics.DENSITY_HIGH:
                return (size * DisplayMetrics.DENSITY_XHIGH) / DisplayMetrics.DENSITY_HIGH;
            case DisplayMetrics.DENSITY_XHIGH:
                return (size * DisplayMetrics.DENSITY_XXHIGH) / DisplayMetrics.DENSITY_XHIGH;
            case DisplayMetrics.DENSITY_XXHIGH:
                return (size * DisplayMetrics.DENSITY_XHIGH * 2) / DisplayMetrics.DENSITY_XXHIGH;
            default:
                return (int) ((size * 1.5f) + .5f);
        }
    }

    public static class TaskDescription {
        private String mLabel;
        private Icon mIcon;
        private String mIconFilename;
        private int mColorPrimary;
        private int mColorBackground;
        private int mColorBackgroundFloating;
        private int mStatusBarColor;
        private int mNavigationBarColor;
        private int mSystemBarsAppearance;
        private int mTopOpaqueSystemBarsAppearance;
        private boolean mEnsureStatusBarContrastWhenTransparent;
        private boolean mEnsureNavigationBarContrastWhenTransparent;
        private int mResizeMode;
        private int mMinWidth;
        private int mMinHeight;

        public static final class Builder {
            private String mLabel = null;
            private int mIconRes = Resources.ID_NULL;
            private int mPrimaryColor = 0;
            private int mBackgroundColor = 0;
            private int mStatusBarColor = 0;
            private int mNavigationBarColor = 0;

            public Builder setLabel(String label) {
                this.mLabel = label;
                return this;
            }

            public Builder setIcon(int iconRes) {
                this.mIconRes = iconRes;
                return this;
            }

            public Builder setPrimaryColor(int color) {
                this.mPrimaryColor = color;
                return this;
            }

            public Builder setBackgroundColor(int color) {
                this.mBackgroundColor = color;
                return this;
            }

            public Builder setStatusBarColor(int color) {
                this.mStatusBarColor = color;
                return this;
            }

            public Builder setNavigationBarColor(int color) {
                this.mNavigationBarColor = color;
                return this;
            }

            public TaskDescription build() {
                final Icon icon = mIconRes == Resources.ID_NULL ? null
                        : Icon.createWithResource(ActivityThread.currentPackageName(), mIconRes);
                return new TaskDescription(mLabel, icon, mPrimaryColor, mBackgroundColor, mStatusBarColor,
                        mNavigationBarColor, 0, 0, false, false, ActivityInfo.RESIZE_MODE_RESIZEABLE, -1, -1, 0);
            }
        }

        public TaskDescription(String label, int iconRes, int colorPrimary) {
            this(label, Icon.createWithResource(ActivityThread.currentPackageName(), iconRes), colorPrimary, 0, 0, 0,
                    0, 0, false, false, ActivityInfo.RESIZE_MODE_RESIZEABLE, -1, -1, 0);
            if ((colorPrimary != 0) && (Color.alpha(colorPrimary) != 255)) {
                throw new RuntimeException("A TaskDescription's primary color should be opaque");
            }
        }

        public TaskDescription(String label, int iconRes) {
            this(label, Icon.createWithResource(ActivityThread.currentPackageName(), iconRes), 0, 0, 0, 0, 0, 0,
                    false, false, ActivityInfo.RESIZE_MODE_RESIZEABLE, -1, -1, 0);
        }

        public TaskDescription(String label) {
            this(label, null, 0, 0, 0, 0, 0, 0, false, false, ActivityInfo.RESIZE_MODE_RESIZEABLE, -1, -1, 0);
        }

        public TaskDescription() {
            this(null, null, 0, 0, 0, 0, 0, 0, false, false, ActivityInfo.RESIZE_MODE_RESIZEABLE, -1, -1, 0);
        }

        public TaskDescription(String label, Bitmap icon, int colorPrimary) {
            this(label, icon != null ? Icon.createWithBitmap(icon) : null, colorPrimary, 0, 0, 0, 0, 0, false, false,
                    ActivityInfo.RESIZE_MODE_RESIZEABLE, -1, -1, 0);
            if ((colorPrimary != 0) && (Color.alpha(colorPrimary) != 255)) {
                throw new RuntimeException("A TaskDescription's primary color should be opaque");
            }
        }

        public TaskDescription(String label, Bitmap icon) {
            this(label, icon != null ? Icon.createWithBitmap(icon) : null, 0, 0, 0, 0, 0, 0, false, false,
                    ActivityInfo.RESIZE_MODE_RESIZEABLE, -1, -1, 0);
        }

        /** @hide */
        public TaskDescription(String label, Icon icon, int colorPrimary, int colorBackground, int statusBarColor,
                int navigationBarColor, int systemBarsAppearance, int topOpaqueSystemBarsAppearance,
                boolean ensureStatusBarContrastWhenTransparent, boolean ensureNavigationBarContrastWhenTransparent,
                int resizeMode, int minWidth, int minHeight, int colorBackgroundFloating) {
            mLabel = label;
            mIcon = icon;
            mColorPrimary = colorPrimary;
            mColorBackground = colorBackground;
            mStatusBarColor = statusBarColor;
            mNavigationBarColor = navigationBarColor;
            mSystemBarsAppearance = systemBarsAppearance;
            mTopOpaqueSystemBarsAppearance = topOpaqueSystemBarsAppearance;
            mEnsureStatusBarContrastWhenTransparent = ensureStatusBarContrastWhenTransparent;
            mEnsureNavigationBarContrastWhenTransparent = ensureNavigationBarContrastWhenTransparent;
            mResizeMode = resizeMode;
            mMinWidth = minWidth;
            mMinHeight = minHeight;
            mColorBackgroundFloating = colorBackgroundFloating;
        }

        public TaskDescription(TaskDescription td) {
            copyFrom(td);
        }

        /** @hide */
        public void copyFrom(TaskDescription other) {
            mLabel = other.mLabel;
            mIcon = other.mIcon;
            mIconFilename = other.mIconFilename;
            mColorPrimary = other.mColorPrimary;
            mColorBackground = other.mColorBackground;
            mStatusBarColor = other.mStatusBarColor;
            mNavigationBarColor = other.mNavigationBarColor;
            mSystemBarsAppearance = other.mSystemBarsAppearance;
            mTopOpaqueSystemBarsAppearance = other.mTopOpaqueSystemBarsAppearance;
            mEnsureStatusBarContrastWhenTransparent = other.mEnsureStatusBarContrastWhenTransparent;
            mEnsureNavigationBarContrastWhenTransparent = other.mEnsureNavigationBarContrastWhenTransparent;
            mResizeMode = other.mResizeMode;
            mMinWidth = other.mMinWidth;
            mMinHeight = other.mMinHeight;
            mColorBackgroundFloating = other.mColorBackgroundFloating;
        }

        /** @hide As copyFrom, but a field the other leaves at its default keeps this one's. */
        public void copyFromPreserveHiddenFields(TaskDescription other) {
            mLabel = other.mLabel;
            mIcon = other.mIcon;
            mIconFilename = other.mIconFilename;
            mColorPrimary = other.mColorPrimary;
            if (other.mColorBackground != 0) {
                mColorBackground = other.mColorBackground;
            }
            if (other.mStatusBarColor != 0) {
                mStatusBarColor = other.mStatusBarColor;
            }
            if (other.mNavigationBarColor != 0) {
                mNavigationBarColor = other.mNavigationBarColor;
            }
            if (other.mSystemBarsAppearance != 0) {
                mSystemBarsAppearance = other.mSystemBarsAppearance;
            }
            if (other.mTopOpaqueSystemBarsAppearance != 0) {
                mTopOpaqueSystemBarsAppearance = other.mTopOpaqueSystemBarsAppearance;
            }
            mEnsureStatusBarContrastWhenTransparent = other.mEnsureStatusBarContrastWhenTransparent;
            mEnsureNavigationBarContrastWhenTransparent = other.mEnsureNavigationBarContrastWhenTransparent;
            if (other.mResizeMode != ActivityInfo.RESIZE_MODE_RESIZEABLE) {
                mResizeMode = other.mResizeMode;
            }
            if (other.mMinWidth != -1) {
                mMinWidth = other.mMinWidth;
            }
            if (other.mMinHeight != -1) {
                mMinHeight = other.mMinHeight;
            }
            if (other.mColorBackgroundFloating != 0) {
                mColorBackgroundFloating = other.mColorBackgroundFloating;
            }
        }

        /** @hide */
        public void setLabel(String label) {
            mLabel = label;
        }

        /** @hide */
        public void setPrimaryColor(int primaryColor) {
            if ((primaryColor != 0) && (Color.alpha(primaryColor) != 255)) {
                throw new RuntimeException("A TaskDescription's primary color should be opaque");
            }
            mColorPrimary = primaryColor;
        }

        /** @hide */
        public void setBackgroundColor(int backgroundColor) {
            if ((backgroundColor != 0) && (Color.alpha(backgroundColor) != 255)) {
                throw new RuntimeException("A TaskDescription's background color should be opaque");
            }
            mColorBackground = backgroundColor;
        }

        /** @hide */
        public void setBackgroundColorFloating(int backgroundColor) {
            if ((backgroundColor != 0) && (Color.alpha(backgroundColor) != 255)) {
                throw new RuntimeException("A TaskDescription's background color floating should be opaque");
            }
            mColorBackgroundFloating = backgroundColor;
        }

        /** @hide */
        public void setStatusBarColor(int statusBarColor) {
            mStatusBarColor = statusBarColor;
        }

        /** @hide */
        public void setNavigationBarColor(int navigationBarColor) {
            mNavigationBarColor = navigationBarColor;
        }

        /** @hide */
        public void setIcon(Icon icon) {
            mIcon = icon;
        }

        /** @hide */
        public void setIconFilename(String iconFilename) {
            mIconFilename = iconFilename;
            if (iconFilename != null) {
                mIcon = null;
            }
        }

        /** @hide */
        public void setResizeMode(int resizeMode) {
            mResizeMode = resizeMode;
        }

        /** @hide */
        public void setMinWidth(int minWidth) {
            mMinWidth = minWidth;
        }

        /** @hide */
        public void setMinHeight(int minHeight) {
            mMinHeight = minHeight;
        }

        public String getLabel() {
            return mLabel;
        }

        /** @hide The icon, or none: a persisted icon is the system's (loadTaskDescriptionIcon). */
        public Icon loadIcon() {
            if (mIcon != null) {
                return mIcon;
            }
            Bitmap loadedIcon = loadTaskDescriptionIcon(mIconFilename, 0);
            return loadedIcon != null ? Icon.createWithBitmap(loadedIcon) : null;
        }

        public Bitmap getIcon() {
            Bitmap icon = getInMemoryIcon();
            return icon != null ? icon : loadTaskDescriptionIcon(mIconFilename, 0);
        }

        /** @hide */
        public Icon getRawIcon() {
            return mIcon;
        }

        /** @hide */
        public String getIconResourcePackage() {
            if (mIcon != null && mIcon.getType() == Icon.TYPE_RESOURCE) {
                return mIcon.getResPackage();
            }
            return "";
        }

        /** @hide */
        public int getIconResource() {
            if (mIcon != null && mIcon.getType() == Icon.TYPE_RESOURCE) {
                return mIcon.getResId();
            }
            return 0;
        }

        /** @hide */
        public String getIconFilename() {
            return mIconFilename;
        }

        /** @hide */
        public Bitmap getInMemoryIcon() {
            if (mIcon != null && mIcon.getType() == Icon.TYPE_BITMAP) {
                return mIcon.getBitmap();
            }
            return null;
        }

        /**
         * @hide A persisted icon, which Android's activity task service reads from its own files: none on Minato,
         * whose activity sets none.
         */
        public static Bitmap loadTaskDescriptionIcon(String iconFilename, int userId) {
            return null;
        }

        public int getPrimaryColor() {
            return mColorPrimary;
        }

        public int getBackgroundColor() {
            return mColorBackground;
        }

        /** @hide */
        public int getBackgroundColorFloating() {
            return mColorBackgroundFloating;
        }

        public int getStatusBarColor() {
            return mStatusBarColor;
        }

        public int getNavigationBarColor() {
            return mNavigationBarColor;
        }

        /** @hide */
        public boolean getEnsureStatusBarContrastWhenTransparent() {
            return mEnsureStatusBarContrastWhenTransparent;
        }

        /** @hide */
        public int getSystemBarsAppearance() {
            return mSystemBarsAppearance;
        }

        /** @hide */
        public int getTopOpaqueSystemBarsAppearance() {
            return mTopOpaqueSystemBarsAppearance;
        }

        /** @hide */
        public void setEnsureStatusBarContrastWhenTransparent(boolean ensureStatusBarContrastWhenTransparent) {
            mEnsureStatusBarContrastWhenTransparent = ensureStatusBarContrastWhenTransparent;
        }

        /** @hide */
        public void setSystemBarsAppearance(int systemBarsAppearance) {
            mSystemBarsAppearance = systemBarsAppearance;
        }

        /** @hide */
        public void setTopOpaqueSystemBarsAppearance(int topOpaqueSystemBarsAppearance) {
            mTopOpaqueSystemBarsAppearance = topOpaqueSystemBarsAppearance;
        }

        /** @hide */
        public boolean getEnsureNavigationBarContrastWhenTransparent() {
            return mEnsureNavigationBarContrastWhenTransparent;
        }

        /** @hide */
        public void setEnsureNavigationBarContrastWhenTransparent(boolean ensureNavigationBarContrastWhenTransparent) {
            mEnsureNavigationBarContrastWhenTransparent = ensureNavigationBarContrastWhenTransparent;
        }

        /** @hide */
        public int getResizeMode() {
            return mResizeMode;
        }

        /** @hide */
        public int getMinWidth() {
            return mMinWidth;
        }

        /** @hide */
        public int getMinHeight() {
            return mMinHeight;
        }
    }
}
