// The hand file of our ActivityManager (ADR-0039 step c2c3, ADR-0045 decision 3): tools/managergen writes the class
// from the public API, whose methods log once and return their default, and the members here are the hidden ones
// that taken AOSP files call, with TaskDescription as AOSP's but its parcel and XML forms. Every themed activity
// keeps its task description in one (Activity.onApplyThemeResource), so TaskDescription holds what it is given.
// No Context gives an ActivityManager yet (ADR-0045's local services); its task description is all c2c3 needs.
// TaskDescription's members, the start codes and getLauncherLargeIconSizeInner follow ActivityManager.java at
// android-15.0.0_r1 (Apache-2.0).
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.ConfigurationInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.drawable.Icon;
import android.os.Handler;
import android.os.IBinder;
import android.os.Process;
import android.os.ServiceManager;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.util.DisplayMetrics;
import android.util.Singleton;
import com.android.minato.services.Unwritten;
import java.io.BufferedReader;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

class ActivityManager {
    /**
     * Ours: Android's asks the activity manager service for the app's earlier exits, which the system keeps across
     * starts (WorkManager's ForceStopRunnable reads them to find a force stop). Minato keeps none, so the list is
     * empty, as it is for an app that never ended.
     */
    public List<ApplicationExitInfo> getHistoricalProcessExitReasons(String packageName, int pid, int maxNum) {
        return new ArrayList<>();
    }

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

    /** @hide The activity manager service, LocalActivityManagerService (ADR-0045 decision 5, step d2), from
     * ServiceManager's "activity" as AOSP's getService takes it. */
    public static IActivityManager getService() {
        return IActivityManagerSingleton.get();
    }

    private static final Singleton<IActivityManager> IActivityManagerSingleton = new Singleton<IActivityManager>() {
        @Override
        protected IActivityManager create() {
            final IBinder b = ServiceManager.getService(Context.ACTIVITY_SERVICE);
            return IActivityManager.Stub.asInterface(b);
        }
    };

    /** @hide */
    public static final int BROADCAST_SUCCESS = 0;

    /** @hide */
    public static final int INTENT_SENDER_BROADCAST = 1;

    /** @hide */
    public static final int INTENT_SENDER_ACTIVITY = 2;

    /** @hide */
    public static final int INTENT_SENDER_SERVICE = 4;

    /** @hide */
    public static final int INTENT_SENDER_FOREGROUND_SERVICE = 5;

    /** @hide As outside system_server on Android: the system is ready by the time an app runs. */
    public static boolean isSystemReady() {
        return true;
    }

    /** @hide As AOSP's: isUserRunning's flag that counts a stopping user as running (UserManager, batch 1b). */
    public static final int FLAG_OR_STOPPED = 1 << 0;

    /** @hide An iPhone is no low-RAM device (ro.config.low_ram), and Minato has no developer option to force it. */
    public static boolean isLowRamDeviceStatic() {
        return false;
    }

    // Gap 1 of the compatibility plan, batch 1: the manager SystemServiceRegistry gives, with decision 4's answers that
    // need no iOS. The process is the app's only one and in front; the memory is the device's, from /proc/meminfo.

    /**
     * ProcessList's memory level of cached apps on a 64-bit device of more than 700 MB with a large display: its
     * highest minfree, 147,456 kB, times 3/2 for 64 bits, which apk-runner-35-bare gives as getMemoryInfo's
     * threshold [verified: services-test's device line, 226,492,416 bytes].
     */
    private static final long CACHED_APP_MEMORY_LEVEL = 147456L * 3 / 2 * 1024;

    private Context mContext;
    private Handler mHandler;

    /** @hide SystemServiceRegistry's, with the outer context and the main thread's handler, as Android's. */
    public ActivityManager(Context context, Handler handler) {
        mContext = context;
        mHandler = handler;
    }

    public boolean isLowRamDevice() {
        return isLowRamDeviceStatic();
    }

    public boolean isBackgroundRestricted() {
        return false;
    }

    public static boolean isRunningInUserTestHarness() {
        return SystemProperties.getBoolean("persist.sys.test_harness", false);
    }

    public static boolean isUserAMonkey() {
        return false;
    }

    /** As AOSP's staticGetMemoryClass: dalvik.vm.heapgrowthlimit in MB, the heap size without it. */
    public int getMemoryClass() {
        String limit = SystemProperties.get("dalvik.vm.heapgrowthlimit", "");
        return !limit.isEmpty() ? megabytes(limit) : getLargeMemoryClass();
    }

    /** As AOSP's staticGetLargeMemoryClass: dalvik.vm.heapsize in MB. */
    public int getLargeMemoryClass() {
        return megabytes(SystemProperties.get("dalvik.vm.heapsize", "16m"));
    }

    private static int megabytes(String size) {
        return Integer.parseInt(size.substring(0, size.length() - 1));
    }

    /**
     * As ActivityTaskManagerService.getDeviceConfigurationInfo: the global configuration's touch screen, keyboard and
     * navigation with their input features, and ro.opengles.version.
     */
    public ConfigurationInfo getDeviceConfigurationInfo() {
        Configuration config = Resources.getSystem().getConfiguration();
        ConfigurationInfo info = new ConfigurationInfo();
        info.reqTouchScreen = config.touchscreen;
        info.reqKeyboardType = config.keyboard;
        info.reqNavigation = config.navigation;
        if (config.navigation == Configuration.NAVIGATION_DPAD
                || config.navigation == Configuration.NAVIGATION_TRACKBALL) {
            info.reqInputFeatures |= ConfigurationInfo.INPUT_FEATURE_FIVE_WAY_NAV;
        }
        if (config.keyboard != Configuration.KEYBOARD_UNDEFINED && config.keyboard != Configuration.KEYBOARD_NOKEYS) {
            info.reqInputFeatures |= ConfigurationInfo.INPUT_FEATURE_HARD_KEYBOARD;
        }
        info.reqGlEsVersion = SystemProperties.getInt("ro.opengles.version",
                ConfigurationInfo.GL_ES_VERSION_UNDEFINED);
        return info;
    }

    /**
     * As ActivityManagerService.getMemoryInfo: the total and the available memory as Process's natives read
     * /proc/meminfo (MemTotal; MemFree with Cached), the threshold of cached apps' level, low below it.
     */
    public void getMemoryInfo(MemoryInfo outInfo) {
        long total = 0;
        long free = 0;
        long cached = 0;
        try (BufferedReader in = new BufferedReader(new FileReader("/proc/meminfo"))) {
            for (String line; (line = in.readLine()) != null; ) {
                if (line.startsWith("MemTotal:")) {
                    total = kilobytes(line);
                } else if (line.startsWith("MemFree:")) {
                    free = kilobytes(line);
                } else if (line.startsWith("Cached:")) {
                    cached = kilobytes(line);
                }
            }
        } catch (IOException | NumberFormatException e) {
            Unwritten.log("android.app.ActivityManager.getMemoryInfo(MemoryInfo) without /proc/meminfo", "zeros");
        }
        outInfo.totalMem = total * 1024;
        outInfo.availMem = (free + cached) * 1024;
        outInfo.threshold = CACHED_APP_MEMORY_LEVEL;
        outInfo.lowMemory = outInfo.availMem < CACHED_APP_MEMORY_LEVEL;
    }

    /** A /proc/meminfo line's value in kB: "MemTotal:       2532020 kB". */
    private static long kilobytes(String line) {
        String value = line.substring(line.indexOf(':') + 1).trim();
        int space = value.indexOf(' ');
        return Long.parseLong(space < 0 ? value : value.substring(0, space));
    }

    /** As ActivityManagerService for an app: its own processes only, here the one, in front. */
    public List<RunningAppProcessInfo> getRunningAppProcesses() {
        ArrayList<RunningAppProcessInfo> processes = new ArrayList<>();
        RunningAppProcessInfo info = new RunningAppProcessInfo(ActivityThread.currentProcessName(), Process.myPid(),
                new String[] {ActivityThread.currentPackageName()});
        info.uid = Process.myUid();
        info.importance = RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
        processes.add(info);
        return processes;
    }

    /** As ActivityManagerService's fillInProcMemInfo for the app's process: in front, never trimmed yet. */
    public static void getMyMemoryState(RunningAppProcessInfo outState) {
        outState.pid = Process.myPid();
        outState.uid = Process.myUid();
        outState.importance = RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
        outState.importanceReasonCode = RunningAppProcessInfo.REASON_UNKNOWN;
        outState.lastTrimLevel = 0;
        outState.lru = 0;
    }

    /** The API's, as AOSP's: nothing to fill in a new one. */
    public static class MemoryInfo {
        public MemoryInfo() {
        }
    }

    /** The API's ctors, as AOSP's, which start an entry in front, and the hidden annotation of its importance. */
    public static class RunningAppProcessInfo {
        /** @hide As AOSP's, without its IntDef: ApplicationExitInfo names the type (batch E of the Unity test APK list). */
        @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
        public @interface Importance {}

        public RunningAppProcessInfo() {
            importance = IMPORTANCE_FOREGROUND;
            importanceReasonCode = REASON_UNKNOWN;
        }

        public RunningAppProcessInfo(String pProcessName, int pPid, String[] pArr) {
            processName = pProcessName;
            pid = pPid;
            pkgList = pArr;
        }
    }

    /**
     * The launcher's large icon size in pixels, 48 dp on a phone [verified: manager-answers-test on apk-runner-35],
     * scaled to the density as getLauncherLargeIconSizeInner does.
     */
    public int getLauncherLargeIconSize() {
        return getLauncherLargeIconSizeInner(mContext);
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

    /**
     * @hide As AOSP's: a subset of immutable pending intent information suitable for caching on the client side, which
     * PendingIntent asks the service for (ADR-0045 step d2, through step d3).
     */
    public static final class PendingIntentInfo implements android.os.Parcelable {

        private final String mCreatorPackage;
        private final int mCreatorUid;
        private final boolean mImmutable;
        private final int mIntentSenderType;

        public PendingIntentInfo(String creatorPackage, int creatorUid, boolean immutable,
                int intentSenderType) {
            mCreatorPackage = creatorPackage;
            mCreatorUid = creatorUid;
            mImmutable = immutable;
            mIntentSenderType = intentSenderType;
        }

        public String getCreatorPackage() {
            return mCreatorPackage;
        }

        public int getCreatorUid() {
            return mCreatorUid;
        }

        public boolean isImmutable() {
            return mImmutable;
        }

        public int getIntentSenderType() {
            return mIntentSenderType;
        }

        @Override
        public int describeContents() {
            return 0;
        }

        @Override
        public void writeToParcel(android.os.Parcel parcel, int flags) {
            parcel.writeString(mCreatorPackage);
            parcel.writeInt(mCreatorUid);
            parcel.writeBoolean(mImmutable);
            parcel.writeInt(mIntentSenderType);
        }

        public static final android.os.Parcelable.Creator<PendingIntentInfo> CREATOR =
                new android.os.Parcelable.Creator<PendingIntentInfo>() {
                    @Override
                    public PendingIntentInfo createFromParcel(android.os.Parcel in) {
                        return new PendingIntentInfo(
                                /* creatorPackage= */ in.readString(),
                                /* creatorUid= */ in.readInt(),
                                /* immutable= */ in.readBoolean(),
                                /* intentSenderType= */ in.readInt());
                    }

                    @Override
                    public PendingIntentInfo[] newArray(int size) {
                        return new PendingIntentInfo[size];
                    }
                };
    }

    /** @hide As AOSP's: the current user, for an app that holds INTERACT_ACROSS_USERS or INTERACT_ACROSS_USERS_FULL,
     * which on Minato is the system user, the only one it runs; any other app gets the SecurityException of
     * UserController's checkGetCurrentUserPermissions [verified: frameworks/base at android-15.0.0_r1]. TextClock asks
     * it when it shows the current user's time (setShowCurrentUserTime). */
    public static int getCurrentUser() {
        Application app = ActivityThread.currentApplication();
        for (String permission : new String[] {"android.permission.INTERACT_ACROSS_USERS",
                "android.permission.INTERACT_ACROSS_USERS_FULL"}) {
            if (app != null && app.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) {
                return UserHandle.USER_SYSTEM;
            }
        }
        throw new SecurityException("Permission Denial: getCurrentUser() from pid=" + Process.myPid() + ", uid="
                + Process.myUid() + " requires android.permission.INTERACT_ACROSS_USERS");
    }
}
