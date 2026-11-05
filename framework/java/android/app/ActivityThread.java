/*
 * The driver of the app process (ADR-0039 decision 5): our class under AOSP's name, which app_process starts with
 * main() on the guest main thread. Libraries reach it by reflection, so it keeps the tag's static getters.
 *
 * Step a's driver is empty: main() records the package and the process name, points TMPDIR and java.io.tmpdir at
 * the app's cache folder as handleBindApplication's setupGraphicsSupport does, builds the app's class loader over
 * base.apk and its lib/arm64, and loads and initializes the manifest's Application class, as makeApplication starts
 * on Android. Before that it gives the system Resources the session's configuration and display metrics (b3.4c),
 * as handleBindApplication does, and reads the app's manifest, from which ApplicationPackageManager answers (b3.5a),
 * as bindApplication hands Android's app its ApplicationInfo, with who signed it from the import's signers.json
 * (b3.5b1). Then it gives hwui the app's target SDK and loads the system's font map (ADR-0043 test 2), as
 * handleBindApplication does. Since step c1 it makes the Application as makeApplication does: through LoadedApk, with
 * the app's ContextImpl attached before attachBaseContext returns. Since step c2b, main() prepares the main Looper as
 * AOSP's does, calls the Application's onCreate as handleBindApplication does, and then runs the Looper, which ends
 * only with the process. Since step c2c1 it installs the process's providers before that onCreate, as
 * handleBindApplication does. Since step c2e it gives ART and libcore the app's target SDK and its disabled compat
 * changes before the app's first class runs, as RuntimeInit and handleBindApplication do, from CompatTable where
 * Android's come from system_server. Since step c2c3 AOSP's Instrumentation calls that onCreate, ActivityClient asks
 * the process's own controller (LocalActivityClientController), and once the bind is done the manifest's launcher
 * activity starts as the system's first launch transaction starts it: performLaunchActivity, handleStartActivity,
 * handleResumeActivity with the window, then the top resumed state. Since ADR-0043 step c1 the host's session drives
 * the activity when the host began one, as activity-host's session drives a NativeActivity (ADR-0028): main()
 * connects to it after the bind and reads its events on the main looper, one at a time, each answered once its
 * callbacks returned; they become Android's lifecycle transactions (CREATE the launch, START, RESUME, PAUSE, STOP and
 * DESTROY as AOSP's handlers, the instance state saved with the stop by the target SDK), and the window's events reach
 * its ViewRootImpl. The last activity's finish() asks the host, which answers with the quit. Without a session the
 * launch runs by itself as before. Since ADR-0045's gap 13a it gives ContentResolver the app's providers and the
 * local settings provider (acquireProvider), whose android_id is the host's --android-id. Since step c2d3 it gives
 * the main thread and the process StrictMode's defaults for the app's target SDK, and allows disk writes while the
 * Application is made and created, as handleBindApplication does. Since step b3.5b2-1 the app starts its own
 * activities in its one task: a start or a finish runs Android's transactions in their order on the main thread,
 * the activities below the top stop once the main thread is idle, the session's lifecycle drives the top activity,
 * and the session's window, input channel and focus go to the top activity's window. Since ADR-0045 step SV2 it makes,
 * starts and destroys the app's started services on the main thread as LocalActiveServices asks, each through the
 * app's AppComponentFactory, as handleCreateService, handleServiceArgs and handleStopService do, and since step SV3
 * binds and unbinds them as handleBindService and handleUnbindService do.
 *
 *   ActivityThread.main --package <package> --process <name> --apk <path of base.apk>
 *                       --library-path <lib/arm64 beside it> --data-dir <the app's data folder>
 *                       [--signers <the version's signers.json>] [--application <class>]
 *                       [--android-id <16 hex digits>]
 */
package android.app;

import android.app.compat.CompatChanges;
import android.content.AttributionSource;
import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.Context;
import android.content.ComponentName;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.type.DefaultMimeMapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.hardware.display.DisplayManagerGlobal;
import android.hardware.display.DisplayService;
import android.net.TrafficStats;
import android.os.AsyncTask;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.LocaleList;
import android.os.Looper;
import android.os.Message;
import android.os.MessageQueue;
import android.os.StrictMode;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.permission.IPermissionManager;
import android.provider.DeviceConfig;
import android.provider.Settings;
import android.provider.SettingsProvider;
import android.security.net.config.NetworkSecurityConfigProvider;
import android.system.ErrnoException;
import android.system.Os;
import android.text.TextFlags;
import android.util.ArrayMap;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.Slog;
import android.util.SuperNotCalledException;
import android.util.TimeUtils;
import android.util.UtilConfig;
import android.view.Display;
import android.view.InputChannel;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewManager;
import android.view.ViewParent;
import android.view.ViewRootImpl;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;
import android.view.WindowSurface;
import android.widget.WidgetFlags;
import com.android.minato.view.SystemBarInsets;
import com.android.externalstorage.ExternalStorageProvider;
import com.android.providers.media.MediaProvider;
import com.android.internal.compat.CompatTable;
import com.android.internal.minato.GapScan;
import com.android.minato.app.ActivityWindows;
import com.android.minato.permissions.PermissionAnswers;
import com.android.minato.permissions.PermissionPolicy;
import com.android.internal.minato.open.OpenIntents;
import com.android.internal.minato.open.SystemOpenActivity;
import com.android.internal.minato.permissions.SystemPermissionActivity;
import android.location.LocalLocationManager;
import com.android.internal.minato.picker.PickerIntents;
import com.android.internal.minato.picker.SystemPickerActivity;
import com.android.internal.minato.picker.UriGrants;
import com.android.internal.os.SafeZipPathValidatorCallback;
import dalvik.system.AppSpecializationHooks;
import dalvik.system.RuntimeHooks;
import dalvik.system.PathClassLoader;
import dalvik.system.VMRuntime;
import dalvik.system.ZipPathValidator;
import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicInteger;
import libcore.content.type.MimeMap;
import org.xmlpull.v1.XmlPullParserException;

public final class ActivityThread {
    /** As AOSP's: off. ResourcesManager logs configuration changes when it is on (b3.4). */
    public static final boolean DEBUG_CONFIGURATION = false;

    /** As AOSP's: BroadcastReceiver logs with them. */
    public static final String TAG = "ActivityThread";
    public static final boolean DEBUG_BROADCAST = false;

    private static volatile ActivityThread sCurrentActivityThread;

    /**
     * The main thread's Handler, to which SharedPreferencesImpl posts its listeners' calls from another thread. main()
     * sets it with the main Looper, as AOSP's does (step c2b).
     */
    static volatile Handler sMainThreadHandler;

    private final String mPackageName;
    private final String mProcessName;
    private final ClassLoader mClassLoader;
    private final ManifestPackage mPackage;
    private LoadedApk mLoadedApk;
    private Application mInitialApplication;
    /** The core settings, as the bind hands them (coreSettings). */
    private final Bundle mCoreSettings = coreSettings();
    /** AOSP's Instrumentation, made at the bind as handleBindApplication makes it (step c2c3). */
    Instrumentation mInstrumentation;
    /** The process's activity client controller (step c2c3), which the activity task service hands out too. */
    LocalActivityClientController mActivityClientController;

    /** The app's providers by authority, as installProvider registers them for acquireProvider (step c2d). */
    final ArrayMap<String, ContentProvider> mLocalProvidersByAuthority = new ArrayMap<>();

    /** The host's android_id (--android-id), or null; the settings provider then makes one (ADR-0045, gap 13a). */
    private String mAndroidId;
    /** The local settings provider, made at its first acquire; under mLocalProvidersByAuthority's lock. */
    private SettingsProvider mSettingsProvider;
    private ExternalStorageProvider mExternalStorageProvider;
    private MediaProvider mMediaProvider;

    /**
     * The activities by their token, as AOSP's ActivityThread keeps them, which Application.getAutofillClient walks:
     * none until step c2c launches the first.
     */
    final ArrayMap<IBinder, ActivityClientRecord> mActivities = new ArrayMap<>();

    /** The lifecycle states a record passes, as ActivityLifecycleItem numbers them. */
    static final int ON_CREATE = 1;
    static final int ON_START = 2;
    static final int ON_RESUME = 3;
    static final int ON_PAUSE = 4;
    static final int ON_STOP = 5;
    static final int ON_DESTROY = 6;

    /** An activity's record, with what AOSP's Application reads; the launch makes and fills it (step c2c3). */
    public static final class ActivityClientRecord {
        IBinder token;
        Intent intent;
        ActivityInfo activityInfo;
        LoadedApk packageInfo;
        /** What the activity's window adds to the process's configuration: its WindowConfiguration. */
        Configuration overrideConfig;
        Activity activity;
        Window window;
        int state;
        /** AOSP's r.state: the instance state the last stop saved, until the next resume. */
        Bundle savedState;
        boolean isTopResumedActivity;
        boolean lastReportedTopResumedState;
        /** The orientation the activity asks for, from its manifest until it sets one. */
        int requestedOrientation;
        /** The activity that waits for its result, with that one's embedded ID and request code (step b3.5b2-2a). */
        ActivityClientRecord resultTo;
        String resultWho;
        int requestCode;
        /** The results that came while it was not resumed, which its next resume delivers before onResume. */
        ArrayList<Result> pendingResults;
        /** The intents that came while it was stopped, which its next resume delivers after its results (b3.5b2-2b). */
        ArrayList<Intent> pendingIntents;
        /** Whether its finish ran, as system_server's ActivityRecord.finishing. */
        boolean finishRequested;
        /**
         * When its launch began, in elapsedRealtime, as ActivityMetricsLogger's launching state notes it: the start of
         * the Displayed line that its window's first draw logs (reportWindowsDrawn); 0 once logged.
         */
        long launchStartTime;
        /** Which window it has (ADR-0041 gap 4 (b3)), as ActivityWindows' pass gave it. */
        final ActivityWindows.Slot windows = new ActivityWindows.Slot();
        /** Whether its windows were given with its own window added, which its resume adds after its create. */
        boolean windowsApplied;
        /** convertToTranslucent's: onTranslucentConversionComplete after the next pass. */
        boolean conversionPending;
        /** A relaunch's: the state the new instance's onCreate and onRestoreInstanceState get, until its start ran. */
        Bundle restoredState;
        /** A relaunch's: what the old instance kept for getLastNonConfigurationInstance, until the new one attached. */
        Activity.NonConfigurationInstances lastNonConfigurationInstances;
    }

    /** A result for an activity, as AOSP's ResultInfo: the embedded ID it was asked for, its request and its answer. */
    private static final class Result {
        final String who;
        final int requestCode;
        final int resultCode;
        final Intent data;

        Result(String who, int requestCode, int resultCode, Intent data) {
            this.who = who;
            this.requestCode = requestCode;
            this.resultCode = resultCode;
            this.data = data;
        }
    }

    private ActivityThread(String packageName, String processName, ClassLoader classLoader, ManifestPackage pkg) {
        mPackageName = packageName;
        mProcessName = processName;
        mClassLoader = classLoader;
        mPackage = pkg;
    }

    /** The app's package as its manifest declares it (b3.5a). */
    ManifestPackage getManifestPackage() {
        return mPackage;
    }

    /** The app's loaded package, from the bind on (step c2c1.1); ApplicationPackageManager reads its Resources. */
    LoadedApk getLoadedApk() {
        return mLoadedApk;
    }

    public static ActivityThread currentActivityThread() {
        return sCurrentActivityThread;
    }

    /** The package name, which Android also uses as the app ops package. */
    public static String currentOpPackageName() {
        return currentPackageName();
    }

    public static String currentPackageName() {
        ActivityThread thread = sCurrentActivityThread;
        return thread != null ? thread.mPackageName : null;
    }

    public static String currentProcessName() {
        ActivityThread thread = sCurrentActivityThread;
        return thread != null ? thread.mProcessName : null;
    }

    /** Null until step c's driver creates the Application. */
    public static Application currentApplication() {
        ActivityThread thread = sCurrentActivityThread;
        return thread != null ? thread.mInitialApplication : null;
    }

    /** As AOSP's: the Application's attribution source, null while Minato's contexts give none (ADR-0045, 13a). */
    public static AttributionSource currentAttributionSource() {
        ActivityThread thread = sCurrentActivityThread;
        return thread != null && thread.mInitialApplication != null
                ? thread.mInitialApplication.getAttributionSource() : null;
    }

    public Application getApplication() {
        return mInitialApplication;
    }

    /**
     * The package service, which Android's binds through ServiceManager. Minato's PackageManager answers from the
     * manifest without it (decision 7), so it is null until step c's local services (ADR-0045); only PackageManager's
     * hidden static helpers call it, which AOSP's ApplicationPackageManager uses and ours does not (b3.5a).
     */
    public static IPackageManager getPackageManager() {
        return null;
    }

    /** The permission service, which Android's binds through ServiceManager: null, as the package service is. */
    public static IPermissionManager getPermissionManager() {
        return null;
    }

    /**
     * A core setting, which Android's activity manager sends each app process (CoreSettingsObserver), for AppGlobals:
     * the core settings below (ADR-0043 step d)), or `defaultValue` where the process has none, as AOSP's. Settings'
     * entries are not among them and answer their callers' defaults: for ViewConfiguration's input timeouts, a long
     * press after 400 ms, a multi-press within 300 ms, and key repeat after 400 ms every 50 ms.
     */
    public int getIntCoreSetting(String key, int defaultValue) {
        return mCoreSettings.getInt(key, defaultValue);
    }

    /** As getIntCoreSetting. */
    public float getFloatCoreSetting(String key, float defaultValue) {
        return mCoreSettings.getFloat(key, defaultValue);
    }

    /**
     * The core settings that Android's activity manager hands the process at the bind (handleBindApplication's
     * mCoreSettings): CoreSettingsObserver's DeviceConfig entries, Editor's cursor and magnifier flags in the
     * "widget" namespace and TextView's flags in the "text" namespace, each from DeviceConfig with its default, where
     * a text flag's default is the build's aconfig value (CoreSettingsObserver.java at android-15.0.0_r1). So
     * TextView measures a line by its glyphs' bounds, as use_bounds_for_width is on. Its Settings entries (long
     * press, key repeat, TIME_12_24, the graphics driver's) are not in it, though Settings came with ADR-0045's gap
     * 13a: they need the settings provider, which comes with the application's context after the bundle, so their
     * readers take their defaults.
     */
    private static Bundle coreSettings() {
        Bundle settings = new Bundle();
        String widget = DeviceConfig.NAMESPACE_WIDGET;
        putFlag(settings, widget, WidgetFlags.ENABLE_CURSOR_DRAG_FROM_ANYWHERE,
                WidgetFlags.KEY_ENABLE_CURSOR_DRAG_FROM_ANYWHERE, WidgetFlags.ENABLE_CURSOR_DRAG_FROM_ANYWHERE_DEFAULT);
        settings.putInt(WidgetFlags.KEY_CURSOR_DRAG_MIN_ANGLE_FROM_VERTICAL,
                DeviceConfig.getInt(widget, WidgetFlags.CURSOR_DRAG_MIN_ANGLE_FROM_VERTICAL,
                        WidgetFlags.CURSOR_DRAG_MIN_ANGLE_FROM_VERTICAL_DEFAULT));
        settings.putInt(WidgetFlags.KEY_FINGER_TO_CURSOR_DISTANCE, DeviceConfig.getInt(widget,
                WidgetFlags.FINGER_TO_CURSOR_DISTANCE, WidgetFlags.FINGER_TO_CURSOR_DISTANCE_DEFAULT));
        putFlag(settings, widget, WidgetFlags.ENABLE_INSERTION_HANDLE_GESTURES,
                WidgetFlags.KEY_ENABLE_INSERTION_HANDLE_GESTURES, WidgetFlags.ENABLE_INSERTION_HANDLE_GESTURES_DEFAULT);
        settings.putInt(WidgetFlags.KEY_INSERTION_HANDLE_DELTA_HEIGHT, DeviceConfig.getInt(widget,
                WidgetFlags.INSERTION_HANDLE_DELTA_HEIGHT, WidgetFlags.INSERTION_HANDLE_DELTA_HEIGHT_DEFAULT));
        settings.putInt(WidgetFlags.KEY_INSERTION_HANDLE_OPACITY, DeviceConfig.getInt(widget,
                WidgetFlags.INSERTION_HANDLE_OPACITY, WidgetFlags.INSERTION_HANDLE_OPACITY_DEFAULT));
        settings.putFloat(WidgetFlags.KEY_LINE_SLOP_RATIO, DeviceConfig.getFloat(widget,
                WidgetFlags.LINE_SLOP_RATIO, WidgetFlags.LINE_SLOP_RATIO_DEFAULT));
        putFlag(settings, widget, WidgetFlags.ENABLE_NEW_MAGNIFIER, WidgetFlags.KEY_ENABLE_NEW_MAGNIFIER,
                WidgetFlags.ENABLE_NEW_MAGNIFIER_DEFAULT);
        settings.putFloat(WidgetFlags.KEY_MAGNIFIER_ZOOM_FACTOR, DeviceConfig.getFloat(widget,
                WidgetFlags.MAGNIFIER_ZOOM_FACTOR, WidgetFlags.MAGNIFIER_ZOOM_FACTOR_DEFAULT));
        settings.putFloat(WidgetFlags.KEY_MAGNIFIER_ASPECT_RATIO, DeviceConfig.getFloat(widget,
                WidgetFlags.MAGNIFIER_ASPECT_RATIO, WidgetFlags.MAGNIFIER_ASPECT_RATIO_DEFAULT));
        putFlag(settings, TextFlags.NAMESPACE, TextFlags.ENABLE_NEW_CONTEXT_MENU, TextFlags.KEY_ENABLE_NEW_CONTEXT_MENU,
                TextFlags.ENABLE_NEW_CONTEXT_MENU_DEFAULT);
        for (int i = 0; i < TextFlags.TEXT_ACONFIGS_FLAGS.length; i++) {
            String flag = TextFlags.TEXT_ACONFIGS_FLAGS[i];
            putFlag(settings, TextFlags.NAMESPACE, flag, TextFlags.getKeyForFlag(flag),
                    TextFlags.TEXT_ACONFIG_DEFAULT_VALUE[i]);
        }
        return settings;
    }

    /** A boolean entry, which CoreSettingsObserver writes as 1 or 0. */
    private static void putFlag(Bundle settings, String namespace, String flag, String key, boolean defaultValue) {
        settings.putInt(key, DeviceConfig.getBoolean(namespace, flag, defaultValue) ? 1 : 0);
    }

    /** The broadcast the main thread is receiving, which StrictMode names in a violation: none, Minato has none yet. */
    public static Intent getIntentBeingBroadcast() {
        return null;
    }

    /** Whether this is system_server's thread: never in an app process. WindowConfiguration asks it. */
    public static boolean isSystem() {
        return false;
    }

    // IDisplayManager.Stub's @EnforcePermission constructor names it; no app process runs that constructor.
    public ContextImpl getSystemContext() {
        throw new UnsupportedOperationException("Minato has no system context (ADR-0045)");
    }

    /**
     * GapScan's lines for `apk` under the tag gapscan, as gapscan-test logs those of an APK it is given: a line with
     * their count, then the summary and the missing classes, methods and fields by package, between a line at the
     * start and one with the time the scan took, which the interpreter makes long for an app of several big dex files.
     */
    private static void scanGaps(String packageName, String apk) {
        long start = SystemClock.uptimeMillis();
        Log.i("gapscan", "scanning base.apk of " + packageName);
        try {
            GapScan.Report report = GapScan.scanApk(apk);
            Log.i("gapscan", "base.apk of " + packageName + ": " + report.lines.size() + " lines");
            for (String line : report.lines) {
                Log.i("gapscan", line);
            }
        } catch (Throwable t) {
            Log.w("gapscan", "the scan of " + apk + " failed", t);
        }
        Log.i("gapscan", "done after " + (SystemClock.uptimeMillis() - start) + " ms");
    }

    // Looper.prepareMainLooper and AsyncTask's executors, deprecated for apps, which only this main calls.
    @SuppressWarnings("deprecation")
    public static void main(String[] args) throws ErrnoException {
        // As RuntimeInit.commonInit sets them up for every app before ActivityThread.main (step c1), with
        // TrafficStats' socket tagger (the Unity test APK probe's blocker 9).
        LoggingHandler logging = new LoggingHandler();
        RuntimeHooks.setUncaughtExceptionPreHandler(logging);
        Thread.setDefaultUncaughtExceptionHandler(new KillApplicationHandler(logging));
        // The default time zone is the device's, which the runtime puts in persist.sys.timezone (the compatibility
        // plan's gap 21).
        RuntimeHooks.setTimeZoneIdSupplier(() -> SystemProperties.get("persist.sys.timezone"));
        TrafficStats.attachSocketTagger();
        // The system bars' sizes the host passed in the environment, which com.android.minato.view cannot read itself.
        SystemBarInsets.initial(barInsets());
        // RuntimeInit.commonInit's MIME table: the extensions of framework.jar's res/*.mime.types.
        MimeMap.setDefaultSupplier(DefaultMimeMapFactory::create);
        String packageName = null;
        String processName = null;
        String apk = null;
        String libraryPath = null;
        String application = null;
        String dataDir = null;
        String signers = null;
        String androidId = null;
        String localTestingDir = null;
        ArrayList<String> splits = new ArrayList<>();
        for (int i = 0; i + 1 < args.length; i += 2) {
            switch (args[i]) {
                case "--package": packageName = args[i + 1]; break;
                case "--process": processName = args[i + 1]; break;
                case "--apk": apk = args[i + 1]; break;
                case "--split": splits.add(args[i + 1]); break;
                case "--library-path": libraryPath = args[i + 1]; break;
                case "--application": application = args[i + 1]; break;
                case "--data-dir": dataDir = args[i + 1]; break;
                case "--signers": signers = args[i + 1]; break;
                case "--android-id": androidId = args[i + 1]; break;
                case "--local-testing-dir": localTestingDir = args[i + 1]; break;
                default: throw new IllegalArgumentException("ActivityThread.main: unknown argument " + args[i]);
            }
        }
        if (packageName == null || apk == null || libraryPath == null || dataDir == null || args.length % 2 != 0
                || androidId != null && !androidId.matches("[0-9a-f]{16}")) {
            throw new IllegalArgumentException(
                    "usage: ActivityThread.main --package <package> [--process <name>] --apk <base.apk> "
                    + "[--split <split.apk>]... --library-path <folder> --data-dir <folder> [--signers <signers.json>] "
                    + "[--application <class>] [--android-id <16 hex digits>] [--local-testing-dir <folder>]");
        }
        // As AOSP's main: the main thread's Looper, and the Handler that framework classes post to it with.
        Looper.prepareMainLooper();
        sMainThreadHandler = new Handler(Looper.myLooper());
        // Context.getCacheDir's folder, which installd creates at the install.
        String tmpdir = dataDir + "/cache";
        System.setProperty("java.io.tmpdir", tmpdir);
        Os.setenv("TMPDIR", tmpdir, true);
        applySessionConfiguration();
        // The import's signers.json beside the version (ADR-0034 decision 2, b3.5b1); an unsigned app has none. The
        // folder of the app's kept pack slices under its external files folder (ADR-0050 decision 2); none without.
        ManifestPackage pkg;
        try {
            pkg = ManifestPackage.read(apk, libraryPath, dataDir, signers, localTestingDir,
                    splits.toArray(new String[0]));
        } catch (IOException | XmlPullParserException e) {
            throw new IllegalStateException("ActivityThread.main: cannot read " + apk + "'s manifest", e);
        }
        if (!pkg.packageName.equals(packageName)) {
            throw new IllegalArgumentException("ActivityThread.main: " + apk + " is " + pkg.packageName + "'s, not "
                    + packageName + "'s");
        }
        // As RuntimeInit.applicationInit before main (step c2e): ART's target SDK and the app's disabled compat
        // changes, before the app's first class runs. Then handleBindApplication's first steps: libcore's
        // Compatibility answers from those changes through AppCompatCallbacks, libcore and ICU apply theirs, and the
        // zip path validator follows the target; then the target's other readers, in AOSP's order. system_server
        // computes the changes from its compat config; CompatTable holds what it would, without overrides.
        int targetSdk = pkg.applicationInfo(0).targetSdkVersion;
        long[] disabledChanges = CompatTable.disabledFor(targetSdk);
        VMRuntime.getRuntime().setTargetSdkVersion(targetSdk);
        VMRuntime.getRuntime().setDisabledCompatChanges(disabledChanges);
        // As handleBindApplication before them: the process's start times, which apps read and the launcher's
        // Displayed line counts from. Minato's host starts the process itself, so its request is its start.
        final long startElapsed = SystemClock.elapsedRealtime();
        final long startUptime = SystemClock.uptimeMillis();
        android.os.Process.setStartTimes(startElapsed, startUptime, startElapsed, startUptime);
        AppCompatCallbacks.install(disabledChanges, CompatTable.loggableFor(targetSdk));
        AppSpecializationHooks.handleCompatChangesBeforeBindingApplication();
        initZipPathValidatorCallback();
        if (targetSdk <= Build.VERSION_CODES.HONEYCOMB_MR1) {
            AsyncTask.setDefaultExecutor(AsyncTask.THREAD_POOL_EXECUTOR);
        }
        UtilConfig.setThrowExceptionForUpperArrayOutOfBounds(targetSdk >= Build.VERSION_CODES.Q);
        Message.updateCheckRecycle(targetSdk);
        // As handleBindApplication: the target SDK for hwui's rendering, then the system fonts. Android loads the
        // preinstalled font map only when system_server's serialized one fails; with no system_server, ours always
        // loads it, from /system/etc's lists and /system/fonts. AOSP does both before it applies the resources'
        // configuration, and ours after, since ManifestPackage.read, which gives the target SDK, reads the manifest
        // through Resources; neither reads that configuration.
        android.graphics.Compatibility.setTargetSdkVersion(targetSdk);
        Typeface.loadPreinstalledSystemFontMap();
        // As handleBindApplication (step c2d3): StrictMode's defaults for the app. On a user build, and for an app that
        // is no bundled system app, they only forbid network on the main thread and file:// URIs that leave the app,
        // each by its target SDK.
        StrictMode.initThreadDefaults(pkg.applicationInfo(0));
        StrictMode.initVmDefaults(pkg.applicationInfo(0));
        // Minato's gap scanner, a developer diagnostic: with MINATO_GAPSCAN=1 in the environment (GuestStart's extra
        // entries), the process scans its own base.apk, not its splits, before the app's first class runs, as on the
        // iPhone no other process can read it, and logs what the platform lacks under the tag gapscan.
        if ("1".equals(System.getenv("MINATO_GAPSCAN"))) {
            scanGaps(packageName, apk);
        }
        ClassLoader loader = new PathClassLoader(pkg.codePaths(), libraryPath,
                ClassLoader.getSystemClassLoader());
        ActivityThread thread = new ActivityThread(packageName, processName != null ? processName : packageName,
                loader, pkg);
        thread.mAndroidId = androidId;
        sCurrentActivityThread = thread;
        // As handleBindApplication: disk writes are allowed while the Application is made and created, and the policy
        // before them comes back afterwards as AOSP's rule has it, whose equals on a fresh ThreadPolicy never holds.
        final StrictMode.ThreadPolicy savedPolicy = StrictMode.allowThreadDiskWrites();
        final StrictMode.ThreadPolicy writesAllowedPolicy = StrictMode.getThreadPolicy();
        try {
            // As handleBindApplication's makeApplicationInner (steps c1 and c1.1): the manifest's Application class,
            // else android.app.Application, unless --application names one, through the app's AppComponentFactory,
            // which loads and initializes it, then its attachBaseContext with the app's ContextImpl.
            // currentApplication() gives it out from then on.
            LoadedApk packageInfo = new LoadedApk(thread, pkg.applicationInfo(0), loader);
            packageInfo.setFeatureSplits(pkg.featureSplits());
            thread.mLoadedApk = packageInfo;
            // As handleBindApplication, before the app's code is loaded: the Network Security Config Provider, which
            // gives the app's TLS objects the trust manager of its network_security_config.xml (or Android's defaults
            // for its target SDK) and libcore its clear text policy (step d).
            NetworkSecurityConfigProvider.install(ContextImpl.createAppContext(thread, packageInfo));
            Application app = packageInfo.makeApplication(application);
            thread.mInitialApplication = app;
            // ADR-0057: the kept URI grants of the earlier starts are read, and the clones of picks that no kept grant
            // names are removed, once and before any pick can make a clone of this start.
            UriGrants.startUp();
            // As handleBindApplication: AOSP's Instrumentation for the process, and the activity client controller,
            // which Android's first launch transaction hands over; Minato's is the process's own (step c2c3).
            thread.mInstrumentation = new Instrumentation();
            thread.mInstrumentation.basicInit(thread);
            thread.mActivityClientController = new LocalActivityClientController(thread);
            ActivityClient.setActivityClientController(thread.mActivityClientController);
            // As handleBindApplication: the process's providers, before the Application's onCreate (step c2c1).
            thread.installContentProviders(app, packageInfo, pkg.processProviders(thread.mProcessName));
            // As handleBindApplication after the providers: Instrumentation.callApplicationOnCreate, and Android's
            // wrapping of what it throws.
            try {
                thread.mInstrumentation.callApplicationOnCreate(app);
            } catch (Exception e) {
                if (!thread.mInstrumentation.onException(app, e)) {
                    throw new RuntimeException("Unable to create application " + app.getClass().getName() + ": " + e,
                            e);
                }
            }
        } finally {
            if (targetSdk < Build.VERSION_CODES.O_MR1
                    || StrictMode.getThreadPolicy().equals(writesAllowedPolicy)) {
                StrictMode.setThreadPolicy(savedPolicy);
            }
        }
        // The system's launch transaction comes after the bind, so the app's own posts from Application.onCreate run
        // first [inferred]: the session's events are posted to the main thread too.
        thread.attachSession();
        // Android's system server reports an app whose main thread stops answering (ANR); Minato's app does it itself.
        com.android.internal.minato.AnrWatchdog.start(sMainThreadHandler, packageName);
        // As AOSP's main: the Looper runs the app's messages until the process ends. Android ends it from outside (am
        // force-stop, a kill); so do Minato's host and the checks on the Mac (framework/tests/app_process_run.py).
        Looper.loop();
        throw new RuntimeException("Main thread loop unexpectedly exited");
    }

    /**
     * As AOSP's (step c2e): the zip path validator that refuses an entry leaving its folder, for an app whose target
     * SDK enables VALIDATE_ZIP_PATH_FOR_PATH_TRAVERSAL, after AppCompatCallbacks, which answers that question.
     */
    private static void initZipPathValidatorCallback() {
        if (CompatChanges.isChangeEnabled(SafeZipPathValidatorCallback.VALIDATE_ZIP_PATH_FOR_PATH_TRAVERSAL)) {
            ZipPathValidator.setCallback(new SafeZipPathValidatorCallback());
        } else {
            ZipPathValidator.clearCallback();
        }
    }

    /**
     * As installContentProviders and installProvider (step c2c1): each provider through the app's AppComponentFactory,
     * attached to the Application, which calls its onCreate, and kept by its authorities. Android's then publishes
     * them to system_server, which Minato has none of. A provider of a split or with attribution tags gets the
     * Application's context here, as Minato's contexts have neither yet (M5, and c2a's gap of attribution contexts).
     */
    private void installContentProviders(Context context, LoadedApk packageInfo, List<ProviderInfo> providers) {
        for (ProviderInfo info : providers) {
            ContentProvider provider;
            try {
                // A provider of a split gets that split's context (the tag's installProvider), which is the app's
                // unless the app loads its splits in isolation.
                Context providerContext = info.splitName != null ? context.createContextForSplit(info.splitName)
                        : context;
                provider = packageInfo.getAppFactory().instantiateProvider(providerContext.getClassLoader(),
                        info.name);
                provider.attachInfo(providerContext, info);
            } catch (Exception e) {
                throw new RuntimeException("Unable to get provider " + info.name + ": " + e, e);
            }
            for (String authority : info.authority.split(";")) {
                mLocalProvidersByAuthority.put(authority, provider);
            }
        }
    }

    /**
     * As AOSP's acquireProvider, which ContextImpl's ApplicationContentResolver calls (ADR-0045, gap 13a): the app's
     * own providers by authority (step c2c1 installs them), and "settings", the local settings provider, which
     * Android's system_server publishes; it is made at its first acquire, on the caller's application context. Android
     * asks system_server for any other authority, which Minato has none of: null, logged as Android logs it. Each
     * provider is local, for any user and both kinds of reference.
     */
    public final IContentProvider acquireProvider(Context c, String auth, int userId, boolean stable) {
        ContentProvider provider;
        synchronized (mLocalProvidersByAuthority) {
            provider = mLocalProvidersByAuthority.get(auth);
            if (provider == null && Settings.AUTHORITY.equals(auth)) {
                if (mSettingsProvider == null) {
                    Context application = c.getApplicationContext();
                    mSettingsProvider = SettingsProvider.create(application != null ? application : c, mAndroidId);
                }
                provider = mSettingsProvider;
            }
            if (provider == null && ExternalStorageProvider.AUTHORITY.equals(auth)) {
                // ADR-0057 step b: the documents provider of the primary storage, which the system picker's results
                // name; made at its first acquire, on the caller's application context.
                if (mExternalStorageProvider == null) {
                    Context application = c.getApplicationContext();
                    Context context = application != null ? application : c;
                    mExternalStorageProvider = new ExternalStorageProvider();
                    mExternalStorageProvider.attachInfo(context, ExternalStorageProvider.providerInfo(context));
                }
                provider = mExternalStorageProvider;
            }
            if (provider == null && "media".equals(auth)) {
                // ADR-0057 step c: the media provider, which serves the photo picker's URIs for now.
                if (mMediaProvider == null) {
                    Context application = c.getApplicationContext();
                    Context context = application != null ? application : c;
                    mMediaProvider = new MediaProvider();
                    android.content.pm.ProviderInfo info = new android.content.pm.ProviderInfo();
                    info.name = MediaProvider.class.getName();
                    info.authority = "media";
                    info.packageName = "com.android.providers.media.module";
                    info.exported = true;
                    info.applicationInfo = context.getApplicationInfo();
                    mMediaProvider.attachInfo(context, info);
                }
                provider = mMediaProvider;
            }
        }
        if (provider == null) {
            Slog.e(TAG, "Failed to find provider info for " + auth);
            return null;
        }
        return provider.getIContentProvider();
    }

    /** As AOSP's: a local provider stays until the process ends; false for none. */
    public final boolean releaseProvider(IContentProvider provider, boolean stable) {
        return provider != null;
    }

    // The app's started services (ADR-0045 step SV2): LocalActiveServices asks, as system_server's ActiveServices asks
    // Android's app through IApplicationThread, and each request runs on the main thread in the order asked, as H's
    // messages do. AOSP's ActivityThread names the kinds of serviceDoneExecuting.
    public static final int SERVICE_DONE_EXECUTING_ANON = 0;
    public static final int SERVICE_DONE_EXECUTING_START = 1;
    public static final int SERVICE_DONE_EXECUTING_STOP = 2;
    public static final int SERVICE_DONE_EXECUTING_REBIND = 3;
    public static final int SERVICE_DONE_EXECUTING_UNBIND = 4;

    /** The running services by their tokens, and the ServiceInfo each was made from; the main thread's. */
    final ArrayMap<IBinder, Service> mServices = new ArrayMap<>();
    private final ArrayMap<IBinder, ServiceInfo> mServicesData = new ArrayMap<>();

    /**
     * As scheduleReceiver, for a broadcast to a receiver of the manifest (gap 9, alarms and jobs that run): the
     * receiver is made on the main thread through the app's AppComponentFactory and gets the intent.
     */
    void scheduleReceiver(Intent intent, ActivityInfo info) {
        sMainThreadHandler.post(() -> handleReceiver(intent, info));
    }

    /**
     * As AOSP's handleReceiver: the receiver through the factory with its split's class loader, a result that
     * finishes by itself (the activity manager waits for none, an unordered broadcast), onReceive, and the finish of
     * a goAsync() result.
     */
    private void handleReceiver(Intent intent, ActivityInfo info) {
        final LoadedApk packageInfo = mLoadedApk;
        BroadcastReceiver receiver = null;
        try {
            final ClassLoader cl = info.splitName != null ? packageInfo.getSplitClassLoader(info.splitName)
                    : packageInfo.getClassLoader();
            receiver = packageInfo.getAppFactory().instantiateReceiver(cl, info.name, intent);
            intent.setExtrasClassLoader(cl);
            intent.prepareToEnterProcess(isProtectedComponent(info), mInitialApplication.getAttributionSource());
            receiver.setPendingResult(new BroadcastReceiver.PendingResult(0, null, null,
                    BroadcastReceiver.PendingResult.TYPE_UNREGISTERED, false, false, null, UserHandle.myUserId(),
                    intent.getFlags()));
            receiver.onReceive(mInitialApplication.getBaseContext(), intent);
        } catch (Exception e) {
            if (!mInstrumentation.onException(receiver, e)) {
                throw new RuntimeException("Unable to start receiver " + info.name + ": " + e.toString(), e);
            }
        }
        final BroadcastReceiver.PendingResult result = receiver != null ? receiver.getPendingResult() : null;
        if (result != null) {
            result.finish();
        }
    }

    /** As scheduleCreateService. */
    void scheduleCreateService(IBinder token, ServiceInfo info, Intent intent) {
        sMainThreadHandler.post(() -> handleCreateService(token, info, intent));
    }

    /** As scheduleServiceArgs, for one start. */
    void scheduleServiceArgs(IBinder token, int startId, int flags, Intent args) {
        sMainThreadHandler.post(() -> handleServiceArgs(token, startId, flags, args));
    }

    /** As scheduleStopService. */
    void scheduleStopService(IBinder token) {
        sMainThreadHandler.post(() -> handleStopService(token));
    }

    /** As scheduleBindService (step SV3). */
    void scheduleBindService(IBinder token, Intent intent, boolean rebind) {
        sMainThreadHandler.post(() -> handleBindService(token, intent, rebind));
    }

    /** As scheduleUnbindService (step SV3). */
    void scheduleUnbindService(IBinder token, Intent intent) {
        sMainThreadHandler.post(() -> handleUnbindService(token, intent));
    }

    /**
     * As AOSP's handleCreateService: the service through the app's AppComponentFactory with its split's class loader,
     * a context of its own (its split's for a split's service), attached and created.
     */
    private void handleCreateService(IBinder token, ServiceInfo info, Intent intent) {
        final LoadedApk packageInfo = mLoadedApk;
        Service service = null;
        try {
            Application app = mInitialApplication;
            final ClassLoader cl = info.splitName != null ? packageInfo.getSplitClassLoader(info.splitName)
                    : packageInfo.getClassLoader();
            service = packageInfo.getAppFactory().instantiateService(cl, info.name, intent);
            ContextImpl context = ContextImpl.getImpl(service.createServiceBaseContext(this, packageInfo));
            if (info.splitName != null) {
                context = (ContextImpl) context.createContextForSplit(info.splitName);
            }
            context.setOuterContext(service);
            service.attach(context, this, info.name, token, app, ActivityManager.getService());
            service.onCreate();
            mServicesData.put(token, info);
            mServices.put(token, service);
            ActivityManager.getService().serviceDoneExecuting(token, SERVICE_DONE_EXECUTING_ANON, 0, 0, null);
        } catch (Exception e) {
            if (!mInstrumentation.onException(service, e)) {
                throw new RuntimeException("Unable to create service " + info.name + ": " + e.toString(), e);
            }
        }
    }

    /** As AOSP's handleServiceArgs: the start's intent with the service's class loader, then onStartCommand. */
    private void handleServiceArgs(IBinder token, int startId, int flags, Intent args) {
        ServiceInfo info = mServicesData.get(token);
        Service s = mServices.get(token);
        if (s != null) {
            try {
                if (args != null) {
                    args.setExtrasClassLoader(s.getClassLoader());
                    args.prepareToEnterProcess(isProtectedComponent(info), s.getAttributionSource());
                }
                int res = s.onStartCommand(args, flags, startId);
                QueuedWork.waitToFinish();
                ActivityManager.getService().serviceDoneExecuting(token, SERVICE_DONE_EXECUTING_START, startId, res,
                        null);
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException("Unable to start service " + s + " with " + args + ": " + e.toString(),
                            e);
                }
            }
        }
    }

    /**
     * As AOSP's handleBindService: the intent with the service's class loader, then onBind, whose binder the service
     * publishes, or onRebind.
     */
    private void handleBindService(IBinder token, Intent intent, boolean rebind) {
        ServiceInfo info = mServicesData.get(token);
        Service s = mServices.get(token);
        if (s != null) {
            try {
                intent.setExtrasClassLoader(s.getClassLoader());
                intent.prepareToEnterProcess(isProtectedComponent(info), s.getAttributionSource());
                if (!rebind) {
                    IBinder binder = s.onBind(intent);
                    ActivityManager.getService().publishService(token, intent, binder);
                } else {
                    s.onRebind(intent);
                    ActivityManager.getService().serviceDoneExecuting(token, SERVICE_DONE_EXECUTING_REBIND, 0, 0,
                            intent);
                }
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException("Unable to bind to service " + s + " with " + intent + ": "
                            + e.toString(), e);
                }
            }
        }
    }

    /** As AOSP's handleUnbindService: onUnbind, whose answer asks for onRebind at the next binding. */
    private void handleUnbindService(IBinder token, Intent intent) {
        ServiceInfo info = mServicesData.get(token);
        Service s = mServices.get(token);
        if (s != null) {
            try {
                intent.setExtrasClassLoader(s.getClassLoader());
                intent.prepareToEnterProcess(isProtectedComponent(info), s.getAttributionSource());
                boolean doRebind = s.onUnbind(intent);
                if (doRebind) {
                    ActivityManager.getService().unbindFinished(token, intent, doRebind);
                } else {
                    ActivityManager.getService().serviceDoneExecuting(token, SERVICE_DONE_EXECUTING_UNBIND, 0, 0,
                            intent);
                }
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException("Unable to unbind to service " + s + " with " + intent + ": "
                            + e.toString(), e);
                }
            }
        }
    }

    /** As AOSP's handleStopService: onDestroy, then the context's registrations go. */
    private void handleStopService(IBinder token) {
        mServicesData.remove(token);
        Service s = mServices.remove(token);
        if (s != null) {
            try {
                s.onDestroy();
                s.detachAndCleanUp();
                Context context = s.getBaseContext();
                if (context instanceof ContextImpl) {
                    ((ContextImpl) context).scheduleFinalCleanup(s.getClassName(), "Service");
                }
                QueuedWork.waitToFinish();
                ActivityManager.getService().serviceDoneExecuting(token, SERVICE_DONE_EXECUTING_STOP, 0, 0, null);
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException("Unable to stop service " + s + ": " + e.toString(), e);
                }
                Slog.i(TAG, "handleStopService: exception for " + token, e);
            }
        } else {
            Slog.i(TAG, "handleStopService: token=" + token + " not found.");
        }
    }

    /**
     * As AOSP's isProtectedComponent, which StrictMode's unsafe intent launch check asks: a component that is not
     * exported; Minato has no permission service to tell a signature permission's protection.
     */
    private static boolean isProtectedComponent(ComponentInfo info) {
        return StrictMode.vmUnsafeIntentLaunchEnabled() && info != null && !info.exported;
    }

    private static volatile boolean sCrashing;

    /**
     * RuntimeInit's: an exception that no thread catches is logged to the crash buffer with AndroidRuntime's "FATAL
     * EXCEPTION" lines, before any handler the app set for the thread runs.
     */
    private static final class LoggingHandler implements Thread.UncaughtExceptionHandler {
        volatile boolean mTriggered;

        @Override
        public void uncaughtException(Thread t, Throwable e) {
            mTriggered = true;
            if (sCrashing) {
                return;
            }
            StringBuilder message = new StringBuilder("FATAL EXCEPTION: ").append(t.getName()).append("\n");
            String processName = currentProcessName();
            if (processName != null) {
                message.append("Process: ").append(processName).append(", ");
            }
            message.append("PID: ").append(android.os.Process.myPid());
            Log.printlns(Log.LOG_ID_CRASH, Log.ERROR, "AndroidRuntime", message.toString(), e);
        }
    }

    /**
     * RuntimeInit's default handler: the app process ends after an exception no thread catches, as Android ends it.
     * Android's also reports the crash to system_server for its dialog, which Minato has none of.
     */
    private static final class KillApplicationHandler implements Thread.UncaughtExceptionHandler {
        private final LoggingHandler mLoggingHandler;

        KillApplicationHandler(LoggingHandler loggingHandler) {
            mLoggingHandler = loggingHandler;
        }

        @Override
        public void uncaughtException(Thread t, Throwable e) {
            try {
                if (!mLoggingHandler.mTriggered) {
                    mLoggingHandler.uncaughtException(t, e);
                }
                if (sCrashing) {
                    return;
                }
                sCrashing = true;
            } finally {
                android.os.Process.killProcess(android.os.Process.myPid());
                System.exit(10);
            }
        }
    }

    /**
     * The system Resources' configuration and display metrics from the session (ADR-0028's configuration), as
     * handleBindApplication's ResourcesManager.applyConfigurationToResources gives them on Android before the
     * Application class loads. Without a session (a host that began none) they keep their defaults.
     */
    @SuppressWarnings("deprecation") // scaledDensity, which apps still read.
    private static void applySessionConfiguration() {
        Configuration config = new Configuration();
        config.setToDefaults();
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        String locale = nativeSessionConfiguration(config, metrics);
        if (locale == null) {
            return;
        }
        // What AConfiguration does not hold: a touch phone's hidden states (WindowManager's computeScreenConfiguration
        // with no keyboard or navigation device), the font settings' defaults, and the size compatibility bit,
        // which reduceScreenLayout adds as the window manager does.
        config.keyboardHidden = Configuration.KEYBOARDHIDDEN_NO;
        config.hardKeyboardHidden = Configuration.HARDKEYBOARDHIDDEN_YES;
        config.navigationHidden = Configuration.NAVIGATIONHIDDEN_YES;
        config.fontScale = 1;
        config.fontWeightAdjustment = 0;
        config.screenLayout = Configuration.reduceScreenLayout(config.screenLayout,
                Math.max(config.screenWidthDp, config.screenHeightDp),
                Math.min(config.screenWidthDp, config.screenHeightDp));
        config.setLocales(LocaleList.forLanguageTags(locale));
        // As DisplayInfo.getMetricsWithSize gives the display's metrics: with the noncompat fields, from which
        // CompatibilityInfo.applyToDisplayMetrics takes the size.
        metrics.densityDpi = metrics.noncompatDensityDpi = config.densityDpi;
        metrics.density = metrics.noncompatDensity = config.densityDpi * DisplayMetrics.DENSITY_DEFAULT_SCALE;
        metrics.scaledDensity = metrics.noncompatScaledDensity = metrics.density;
        metrics.xdpi = metrics.noncompatXdpi = config.densityDpi;
        metrics.ydpi = metrics.noncompatYdpi = config.densityDpi;
        metrics.noncompatWidthPixels = metrics.widthPixels;
        metrics.noncompatHeightPixels = metrics.heightPixels;
        // updateSystemConfiguration changes the system Resources only when they exist, as zygote's preloadResources
        // has made them on Android; ours makes them here.
        Resources.getSystem();
        Resources.updateSystemConfiguration(config, metrics, null);
    }

    /**
     * Fills {@code config} and {@code metrics} with the session's values that libandroid's AConfiguration holds and
     * returns its locale as a language tag, or null without a session.
     */
    private static native String nativeSessionConfiguration(Configuration config, DisplayMetrics metrics);
    // The launch (step c2c3), as the system's first launch transaction: LaunchActivityItem, then ResumeActivityItem
    // with its start, then TopResumedActivityChangeItem.

    /**
     * Starts the manifest's launcher activity as the home screen does: the launch intent with the launcher's flags,
     * whose component is the alias where the manifest has one, with the window's configuration from the display.
     */
    private void handleLaunchLauncherActivity() {
        ActivityClientRecord r = launchLauncherActivity();
        if (r == null) {
            return;
        }
        handleStartActivity(r);
        handleResumeActivity(r);
        handleTopResumedActivityChanged(r, true);
    }

    /** The manifest's launcher activity, launched up to its onCreate; null without one or when its launch failed. */
    private ActivityClientRecord launchLauncherActivity() {
        ActivityInfo info = mPackage.launcherActivity(PackageManager.GET_SHARED_LIBRARY_FILES);
        if (info == null) {
            Log.w(TAG, mPackageName + " has no launcher activity, so none starts");
            return null;
        }
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_LAUNCHER);
        intent.setComponent(new ComponentName(info.packageName, info.name));
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        mTaskActivity = intent.getComponent();
        ActivityClientRecord r = newRecord(intent, info);
        // A cold launch, which Android times from its request, before the process starts; ours from the start.
        r.launchStartTime = android.os.Process.getStartElapsedRealtime();
        if (performLaunchActivity(r) == null) {
            removeRecord(r);
            return null;
        }
        return r;
    }

    /**
     * A new record on top of the task, with the window's configuration from the display and its launch's start: a
     * launch that makes a new instance is a warm one, which Android's Displayed line times.
     */
    private ActivityClientRecord newRecord(Intent intent, ActivityInfo info) {
        ActivityClientRecord r = new ActivityClientRecord();
        r.token = new Binder();
        r.intent = intent;
        r.activityInfo = info;
        r.packageInfo = mLoadedApk;
        r.overrideConfig = windowConfiguration();
        r.requestedOrientation = info.screenOrientation;
        r.launchStartTime = SystemClock.elapsedRealtime();
        mActivities.put(r.token, r);
        mTask.add(r);
        return r;
    }

    private void removeRecord(ActivityClientRecord r) {
        mTask.remove(r);
        mActivities.remove(r.token);
    }

    /**
     * The configuration an activity's window adds to the process's, as system_server's ActivityRecord gives a
     * fullscreen task's: the display's bounds, which the window covers whole (ADR-0038), as bounds, app bounds and
     * maximum bounds, fullscreen and standard, at the display's rotation.
     */
    @SuppressWarnings("deprecation") // Display.getRealSize, the display's whole size, which the window covers.
    private static Configuration windowConfiguration() {
        Display display = DisplayManagerGlobal.getInstance().getRealDisplay(Display.DEFAULT_DISPLAY);
        Point size = new Point();
        display.getRealSize(size);
        Rect bounds = new Rect(0, 0, size.x, size.y);
        Configuration config = new Configuration();
        config.windowConfiguration.setBounds(bounds);
        config.windowConfiguration.setAppBounds(bounds);
        config.windowConfiguration.setMaxBounds(bounds);
        config.windowConfiguration.setWindowingMode(WindowConfiguration.WINDOWING_MODE_FULLSCREEN);
        config.windowConfiguration.setActivityType(WindowConfiguration.ACTIVITY_TYPE_STANDARD);
        config.windowConfiguration.setRotation(display.getRotation());
        config.windowConfiguration.setDisplayRotation(display.getRotation());
        return config;
    }

    /**
     * As AOSP's performLaunchActivity: the activity's class (the alias's target), its context with the window's
     * configuration, attach, the manifest's theme and onCreate through Instrumentation.
     */
    private Activity performLaunchActivity(ActivityClientRecord r) {
        ActivityInfo aInfo = r.activityInfo;
        ComponentName component = r.intent.getComponent();
        if (aInfo.targetActivity != null) {
            component = new ComponentName(aInfo.packageName, aInfo.targetActivity);
        }
        ContextImpl appContext = ContextImpl.createActivityContext(this, r.packageInfo, aInfo, r.token,
                Display.DEFAULT_DISPLAY, r.overrideConfig);
        Activity activity = null;
        try {
            ClassLoader cl = appContext.getClassLoader();
            activity = mInstrumentation.newActivity(cl, component.getClassName(), r.intent);
            r.intent.setExtrasClassLoader(cl);
        } catch (Exception e) {
            if (!mInstrumentation.onException(activity, e)) {
                throw new RuntimeException("Unable to instantiate activity " + component + ": " + e, e);
            }
        }
        try {
            if (activity != null) {
                CharSequence title = aInfo.loadLabel(appContext.getPackageManager());
                Configuration config = new Configuration(Resources.getSystem().getConfiguration());
                if (r.overrideConfig != null) {
                    config.updateFrom(r.overrideConfig);
                }
                appContext.setOuterContext(activity);
                activity.attach(appContext, this, mInstrumentation, r.token, 0, mInitialApplication, r.intent, aInfo,
                        title, null, null, r.lastNonConfigurationInstances, config, null, null, null, null, null, null,
                        null);
                r.lastNonConfigurationInstances = null;
                activity.mStartedActivity = false;
                int theme = aInfo.getThemeResource();
                if (theme != 0) {
                    activity.setTheme(theme);
                }
                activity.mCalled = false;
                // As AOSP's: the record knows the activity before onCreate, for the lifecycle callbacks.
                r.activity = activity;
                mInstrumentation.callActivityOnCreate(activity, r.restoredState);
                if (!activity.mCalled) {
                    throw new SuperNotCalledException("Activity " + r.intent.getComponent().toShortString()
                            + " did not call through to super.onCreate()");
                }
            }
            r.state = ON_CREATE;
        } catch (SuperNotCalledException e) {
            throw e;
        } catch (Exception e) {
            if (!mInstrumentation.onException(activity, e)) {
                throw new RuntimeException("Unable to start activity " + component + ": " + e, e);
            }
        }
        return activity;
    }

    /**
     * As AOSP's handleStartActivity for a new activity: onStart, then, for a relaunch's saved state,
     * onRestoreInstanceState, then onPostCreate.
     */
    private void handleStartActivity(ActivityClientRecord r) {
        final Activity activity = r.activity;
        final Bundle restored = r.restoredState;
        r.restoredState = null;
        activity.performStart("handleStartActivity");
        r.state = ON_START;
        if (restored != null) {
            mInstrumentation.callActivityOnRestoreInstanceState(activity, restored);
        }
        activity.mCalled = false;
        mInstrumentation.callActivityOnPostCreate(activity, restored);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + r.intent.getComponent().toShortString()
                    + " did not call through to super.onPostCreate()");
        }
    }

    /**
     * As AOSP's handleResumeActivity: onResume, then the window, added invisible and made visible, so that its first
     * traversal attaches and lays out the decor at the next frame.
     */
    private void handleResumeActivity(ActivityClientRecord r) {
        final Activity a = r.activity;
        // As AOSP's resume to the final state: a start for a result before it no longer keeps the window hidden.
        a.mStartedActivity = false;
        a.performResume(false, "RESUME_ACTIVITY");
        r.state = ON_RESUME;
        r.savedState = null;
        reportTopResumedActivityChanged(r, r.isTopResumedActivity, "topWhenResuming");
        final boolean willBeVisible = !a.mStartedActivity;
        if (r.window == null && !a.mFinished && willBeVisible) {
            r.window = a.getWindow();
            View decor = r.window.getDecorView();
            decor.setVisibility(View.INVISIBLE);
            ViewManager wm = a.getWindowManager();
            WindowManager.LayoutParams l = r.window.getAttributes();
            a.mDecor = decor;
            l.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
            l.softInputMode |= WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION;
            if (a.mVisibleFromClient && !a.mWindowAdded) {
                a.mWindowAdded = true;
                wm.addView(decor, l);
            }
        }
        if (!a.mFinished && willBeVisible && a.mDecor != null) {
            a.mVisibleFromServer = true;
            if (a.mVisibleFromClient) {
                a.makeVisible();
            }
        }
    }

    /** As AOSP's handleTopResumedActivityChanged for a resumed activity. */
    private void handleTopResumedActivityChanged(ActivityClientRecord r, boolean onTop) {
        if (r.isTopResumedActivity == onTop) {
            return;
        }
        r.isTopResumedActivity = onTop;
        if (r.state == ON_RESUME && r.lastReportedTopResumedState != onTop) {
            r.lastReportedTopResumedState = onTop;
            r.activity.performTopResumedActivityChanged(onTop, "topStateChangedWhenResumed");
        }
    }

    // The host's session (ADR-0043 step c1): the event types of ADR-0028's pipe (guest_interface.h's
    // apkrt_event_type) and the FINISH request (apkrt_request_type).
    private static final int EVENT_CREATE = 1;
    private static final int EVENT_START = 2;
    private static final int EVENT_RESUME = 3;
    private static final int EVENT_PAUSE = 4;
    private static final int EVENT_STOP = 5;
    private static final int EVENT_DESTROY = 6;
    private static final int EVENT_SAVE_INSTANCE_STATE = 7;
    private static final int EVENT_WINDOW_FOCUS_CHANGED = 8;
    private static final int EVENT_NATIVE_WINDOW_CREATED = 9;
    private static final int EVENT_NATIVE_WINDOW_RESIZED = 10;
    private static final int EVENT_CONFIGURATION_CHANGED = 16;
    private static final int EVENT_NATIVE_WINDOW_REDRAW_NEEDED = 11;
    private static final int EVENT_NATIVE_WINDOW_DESTROYED = 12;
    private static final int EVENT_INPUT_QUEUE_CREATED = 13;
    private static final int EVENT_INPUT_QUEUE_DESTROYED = 14;
    private static final int EVENT_LOW_MEMORY = 17;
    private static final int EVENT_SURFACE_CREATED = 18;
    private static final int EVENT_SURFACE_RESIZED = 19;
    private static final int EVENT_SURFACE_DESTROYED = 20;
    private static final int EVENT_WINDOW_CREATED = 21;
    private static final int EVENT_WINDOW_RESIZED = 22;
    private static final int EVENT_WINDOW_DESTROYED = 23;
    private static final int EVENT_PICK_RESULT = 24;
    private static final int EVENT_OPEN_RESULT = 25;
    private static final int EVENT_PERMISSION_RESULT = 26;
    private static final int EVENT_LOCATION = 27;
    private static final int EVENT_BATTERY = 28;
    private static final int EVENT_IME_INSETS = 29;
    private static final int EVENT_BACK = 30;
    private static final int EVENT_IME_HIDDEN = 31;
    private static final int EVENT_BAR_INSETS = 32;
    private static final int EVENT_NETWORK = 33;
    private static final int EVENT_TRIM_MEMORY = 34;
    private static final int EVENT_NOTIFICATION_TAP = 35;
    private static final int REQUEST_FINISH = 1;
    private static final int REQUEST_WINDOW_FORMAT = 6;
    private static final int REQUEST_BACK_AT_ROOT = 19;
    private static final int REQUEST_ORIENTATION = 20;
    private static final int SURFACE_MAKE = 1;
    private static final int SURFACE_PLACE = 2;
    private static final int SURFACE_REMOVE = 3;
    private static final int WINDOW_MAKE = 1;
    private static final int WINDOW_PLACE = 2;
    private static final int WINDOW_REMOVE = 3;
    /** Linux's EOPNOTSUPP: without a session, a host gives no surfaces. */
    private static final int EOPNOTSUPP = 95;

    /** The session's event pipe; null without a session. */
    private FileDescriptor mSessionEvents;
    /** The activity the app's task was made for, its launcher activity, as Android's Task.realActivity. */
    private ComponentName mTaskActivity;
    /** Whether the session's lifecycle has the app resumed, so that starts and finishes run; true without a session. */
    private boolean mSessionResumed;
    /**
     * The session's window (ID 0 for none) and its size in pixels, its input channel and its focus, as the host gave
     * them last; the window of the activity on top holds them (step b3.5b2-1).
     */
    private long mHostWindow;
    private int mHostWidth;
    private int mHostHeight;
    private InputChannel mHostInput;
    private boolean mHostFocus;
    /**
     * The window that holds the input channel and the focus: the lowest visible activity's (ADR-0041 gap 4 (b3)), null
     * before the first.
     */
    private ViewRootImpl mHostRoot;
    /**
     * The window that holds the session's window: the lowest visible activity's, null below a floating one at the
     * bottom, whose requested window lies over the empty session's window (gap 4 (b3)).
     */
    private ViewRootImpl mWindowRoot;
    /** Which window each activity has (gap 4 (b3), decision 3). */
    private final ActivityWindows mWindows = new ActivityWindows();
    /** The event types answered without more, each logged once: the content's rectangle. */
    private final boolean[] mSkippedEvents = new boolean[32];
    /** Whether the session's turn marker came and its window size has not yet. */
    private boolean mTurnMarked;

    /**
     * Connects to the host's session, as activity-host does at its start, and reads its events from then on. Without a
     * session (a host that began none, such as the Mac's checks without a configuration), the launcher activity starts
     * by itself, as before.
     */
    private void attachSession() {
        FileDescriptor events = nativeSessionConnect();
        if (events == null) {
            mSessionResumed = true;
            synchronized (sEarlyNotifications) {
                sEarlyNotifications.clear();
            }
            sMainThreadHandler.post(this::handleLaunchLauncherActivity);
            return;
        }
        mSessionEvents = events;
        ViewRootImpl.setHostWindows(true);
        // The notifications the Application posted before the session was there (ADR-0074 decision 2), in order.
        synchronized (sEarlyNotifications) {
            for (byte[] text : sEarlyNotifications) {
                nativeSessionRequestNotification(text);
            }
            sEarlyNotifications.clear();
        }
        watchSession();
    }

    /** APKRT_REQUEST_NOTIFICATION's texts of the time before the session (Application.onCreate), at most 64. */
    private static final java.util.ArrayList<byte[]> sEarlyNotifications = new java.util.ArrayList<>();
    private static final int EARLY_NOTIFICATIONS_LIMIT = 64;

    /**
     * Waits for the next event, as activity-host's looper callback does: the pipe leaves the looper while an event is
     * handled, so an app that polls the main looper in a callback gets no next event there, as Android's lifecycle
     * transactions wait for the one before. The event is handled in a message of the main queue, as Android's
     * transactions are, after what the app posted before it.
     */
    private void watchSession() {
        Looper.myQueue().addOnFileDescriptorEventListener(mSessionEvents,
                MessageQueue.OnFileDescriptorEventListener.EVENT_INPUT, (fd, events) -> {
                    long[] event = new long[5];
                    if (!nativeSessionRead(fd, event)) {
                        Log.e(TAG, "The session's event pipe broke");
                        nativeExit(1);
                    }
                    sMainThreadHandler.post(() -> handleSessionEvent(event));
                    return 0;
                });
    }

    /**
     * One event (type, serial and three arguments): the callbacks of Android's lifecycle transaction for it, the
     * answer, then the next event. DESTROY's answer ends the process with status 0, as activity-host's does.
     */
    private void handleSessionEvent(long[] event) {
        final int type = (int) event[0];
        // The lifecycle's events drive the task's top activity that is not finishing (step b3.5b2-1): a finished one
        // goes once the session resumes and its finish runs. The pause and the stop take whichever activity is resumed
        // or paused, also one whose finish has not run yet. The window's events reach the window on top.
        final ActivityClientRecord r = topLiveRecord();
        final ViewRootImpl root = mHostRoot;
        final ViewRootImpl windowRoot = mWindowRoot;
        switch (type) {
            case EVENT_CREATE:
                if (mTask.isEmpty()) {
                    launchLauncherActivity();
                }
                break;
            case EVENT_START:
                if (r != null && r.state == ON_CREATE) {
                    handleStartActivity(r);
                } else if (r != null && r.state == ON_STOP) {
                    // At the return every activity the pass shows starts again, bottom up, as the task's
                    // ensureActivitiesVisible makes them visible: one below a translucent top before the top
                    // (ADR-0041 gap 4 (b5a)).
                    for (ActivityClientRecord shown : new ArrayList<>(mTask)) {
                        if (shown != r && shown.state == ON_STOP && !finishing(shown)
                                && shown.windows.mode() != ActivityWindows.HIDDEN) {
                            performRestartActivity(shown);
                            updateVisibility(shown, true);
                        }
                    }
                    performRestartActivity(r);
                }
                break;
            case EVENT_RESUME:
                mSessionResumed = true;
                // The requested windows the host took away at the background come back (ADR-0041 gap 4 (b5a)).
                WindowManagerGlobal.getInstance().dispatchSessionReturn();
                if (r != null && r.state == ON_START) {
                    deliverPending(r);
                    handleResumeActivity(r);
                    handleTopResumedActivityChanged(r, true);
                    updateWindows();
                }
                runTransactions();
                break;
            case EVENT_PAUSE:
                mSessionResumed = false;
                for (int i = mTask.size() - 1; i >= 0; i--) {
                    ActivityClientRecord resumed = mTask.get(i);
                    if (resumed.state == ON_RESUME) {
                        handlePauseActivity(resumed, !resumed.activity.mFinished);
                    }
                }
                break;
            case EVENT_STOP:
                for (ActivityClientRecord paused : reversedTask()) {
                    if (paused.state == ON_PAUSE) {
                        handleStopActivity(paused);
                    }
                }
                break;
            case EVENT_DESTROY:
                // The app's end: every activity of the task, the top first.
                for (ActivityClientRecord destroyed : reversedTask()) {
                    handleDestroyActivity(destroyed);
                }
                break;
            case EVENT_SAVE_INSTANCE_STATE:
                // As Android's: the stop saved the state, before onStop for a target below 28 and after it from 28 on.
                break;
            case EVENT_WINDOW_FOCUS_CHANGED:
                mHostFocus = event[2] != 0;
                // The session's focus goes to its top focusable window (ADR-0041 gap 4 (b2)).
                WindowManagerGlobal.getInstance().setSessionWindow(root, mHostFocus);
                break;
            case EVENT_CONFIGURATION_CHANGED:
                // The host's turn (ADR-0038: the session's configuration stays): the marker of the window size that
                // the next resize brings, from which the turn's configuration is derived.
                mTurnMarked = true;
                break;
            case EVENT_NATIVE_WINDOW_CREATED:
            case EVENT_NATIVE_WINDOW_RESIZED:
                mHostWindow = event[2];
                mHostWidth = (int) event[3];
                mHostHeight = (int) event[4];
                if (windowRoot != null) {
                    windowRoot.setHostWindow(mHostWindow, mHostWidth, mHostHeight);
                } else if (root != null) {
                    clearSessionWindow();
                }
                if (type == EVENT_NATIVE_WINDOW_RESIZED && mTurnMarked) {
                    mTurnMarked = false;
                    handleTurn(mHostWidth, mHostHeight);
                }
                break;
            case EVENT_NATIVE_WINDOW_REDRAW_NEEDED:
                if (windowRoot != null) {
                    windowRoot.redrawNeeded();
                } else if (root != null) {
                    clearSessionWindow();
                }
                break;
            case EVENT_NATIVE_WINDOW_DESTROYED:
                mHostWindow = 0;
                if (windowRoot != null) {
                    windowRoot.setHostWindow(0, 0, 0);
                }
                break;
            case EVENT_INPUT_QUEUE_CREATED:
            case EVENT_INPUT_QUEUE_DESTROYED:
                // The window's touches (ADR-0045 step 7b2): the session's input channel as an InputChannel, none
                // for channel 0 and at the queue's end; one that comes before a window waits for it (step b3.5b2-1).
                InputChannel channel = null;
                if (type == EVENT_INPUT_QUEUE_CREATED && event[2] != 0) {
                    channel = nativeSessionConnectInput(event[2],
                            root != null ? String.valueOf(root.mWindowAttributes.getTitle()) : mPackageName);
                    if (channel == null) {
                        Log.w(TAG, "The session's input channel " + event[2] + " did not connect: no touches");
                    }
                }
                if (root != null) {
                    root.setInputChannel(channel);
                } else if (mHostInput != null) {
                    mHostInput.dispose();
                }
                mHostInput = channel;
                break;
            case EVENT_SURFACE_CREATED:
                SurfaceView.dispatchSurfaceCreated(event[2], event[3], (int) event[4], (int) (event[4] >>> 32));
                break;
            case EVENT_SURFACE_RESIZED:
                SurfaceView.dispatchSurfaceResized(event[2], (int) event[4], (int) (event[4] >>> 32));
                break;
            case EVENT_SURFACE_DESTROYED:
                SurfaceView.dispatchSurfaceDestroyed(event[2]);
                break;
            // A requested window's, or a SurfaceView's in a requested window (gap 4 (b3), decision 7).
            case EVENT_WINDOW_CREATED:
                if (!WindowManagerGlobal.getInstance().dispatchWindowCreated(event[2], event[3], (int) event[4],
                        (int) (event[4] >>> 32))) {
                    SurfaceView.dispatchSurfaceCreated(event[2], event[3], (int) event[4], (int) (event[4] >>> 32));
                }
                break;
            case EVENT_WINDOW_RESIZED:
                if (!WindowManagerGlobal.getInstance().dispatchWindowResized(event[2], (int) event[4],
                        (int) (event[4] >>> 32))) {
                    SurfaceView.dispatchSurfaceResized(event[2], (int) event[4], (int) (event[4] >>> 32));
                }
                break;
            case EVENT_WINDOW_DESTROYED:
                if (!WindowManagerGlobal.getInstance().dispatchWindowDestroyed(event[2])) {
                    SurfaceView.dispatchSurfaceDestroyed(event[2]);
                }
                break;
            case EVENT_PICK_RESULT:
                SystemPickerActivity.onResult(event[2], (int) event[3], (int) event[4]);
                break;
            case EVENT_OPEN_RESULT:
                SystemOpenActivity.onResult(event[2], (int) event[3], (int) event[4]);
                break;
            case EVENT_PERMISSION_RESULT:
                SystemPermissionActivity.onResult(event[2], (int) event[3], (int) event[4]);
                break;
            case EVENT_LOCATION:
                LocalLocationManager.onEvent(event[2], (int) event[3], event[4]);
                break;
            case EVENT_BATTERY:
                LocalBatteryService.onEvent((int) event[2], (int) event[3]);
                break;
            case EVENT_IME_INSETS:
                // The soft keyboard's height (ADR-0068 step 2d): every window's ime() inset follows it.
                WindowManagerGlobal.getInstance().dispatchImeInsets((int) event[2]);
                break;
            case EVENT_BACK:
                // The system's back (ADR-0041 decision 10), for the window that has the focus.
                WindowManagerGlobal.getInstance().dispatchBack((int) event[2], (int) event[3], (int) event[4]);
                break;
            case EVENT_IME_HIDDEN:
                // The user hid the keyboard (ADR-0068), for the window that has the focus.
                WindowManagerGlobal.getInstance().dispatchImeHidden();
                break;
            case EVENT_BAR_INSETS:
                // The system bars' heights changed at a turn (ADR-0041 decision 8): floating windows opened from now
                // on are laid out between them; the host's values win over the start's environment.
                SystemBarInsets.set((int) event[2], (int) event[3]);
                break;
            case EVENT_NETWORK:
                // The host's network changed (ADR-0054 step d)): ConnectivityManager's callbacks hear of it.
                com.android.internal.minato.LocalNetwork.onEvent((int) event[2]);
                break;
            case EVENT_TRIM_MEMORY:
                // The system trims the app's memory (ComponentCallbacks2.onTrimMemory): Android 15 tells an app of
                // UI_HIDDEN (20) when its UI is gone and of BACKGROUND (40) when it is cached.
                handleTrimMemory((int) event[2]);
                break;
            case EVENT_NOTIFICATION_TAP:
                // A tap on one of the app's notifications (ADR-0074 decision 4): its content intent goes out.
                NotificationService.onHostTap((int) event[2], event[3]);
                break;
            case EVENT_LOW_MEMORY:
                handleLowMemory();
                break;
            default:
                // NativeActivity's content rectangle (ADR-0041 b)) and turns (ADR-0041 decision 9).
                if (type > 0 && type < mSkippedEvents.length && !mSkippedEvents[type]) {
                    mSkippedEvents[type] = true;
                    Log.w(TAG, "The session's event " + type + " is answered without more (ADR-0043 step c1)");
                }
                break;
        }
        nativeSessionAnswer((int) event[1]);
        if (type == EVENT_DESTROY) {
            nativeExit(0);
        }
        watchSession();
    }

    // The app's one task (step b3.5b2-1). Android's activity task manager keeps an app's activities in tasks and drives
    // them with lifecycle transactions; Minato runs one app per process and has no recents, so all its activities
    // share one task, also those started with FLAG_ACTIVITY_NEW_TASK, which Android gives a task of their own only for
    // another affinity (a named departure). A start or a finish, asked from any thread, becomes a transaction on the
    // main thread, run in the order asked while the session has the app resumed: the top activity pauses, the next one
    // comes up to its resume and takes the session's window, and once the main thread is idle the activities below the
    // top stop and the finished ones go, as Android stops them once the new top reports idle. Since step b3.5b2-2a an
    // activity started for a result returns it to the one that waits for it; since step b3.5b2-2b a start follows the
    // launch modes and the task's flags, which can finish activities in the task and give one of them the intent.

    /** The task's activities, its root first and its top last; a launch adds one, a destroy removes it. */
    private final ArrayList<ActivityClientRecord> mTask = new ArrayList<>();
    /** The starts and finishes asked for and not run yet, in their order; the main thread's. */
    private final ArrayDeque<Runnable> mTransactions = new ArrayDeque<>();
    /** Starts asked for, from any thread, whose launch has not run: the last activity's finish waits for them. */
    private final AtomicInteger mPendingStarts = new AtomicInteger();
    /** Whether an idle pass over the activities below the top is due. */
    private boolean mIdleScheduled;
    /** The results of a transaction's finishes for resumed activities, sent at its end, as finishActivityResults posts. */
    private final ArrayList<Runnable> mResultsToResumed = new ArrayList<>();

    /**
     * LocalActivityTaskManager's start, from any thread: Android's answer for the intent at once, the launch on the
     * main thread later. Only an explicit component of the app's manifest starts, as no other app is installed: an
     * implicit intent resolves with step b3.5b2-3. As ActivityStarter.executeRequest, a start for a result from an
     * activity (`resultTo`) makes that one wait for the result, and FLAG_ACTIVITY_FORWARD_RESULT hands on the activity
     * that waits for the starting one's, which a request code of the start's own contradicts (step b3.5b2-2a).
     */
    int startActivity(Intent intent, IBinder resultTo, String resultWho, int requestCode) {
        final ActivityClientRecord source = resultTo != null ? mActivities.get(resultTo) : null;
        final boolean forward = source != null && (intent.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0;
        if (forward && requestCode >= 0) {
            return ActivityManager.START_FORWARD_AND_REQUEST_CONFLICT;
        }
        ComponentName component = intent.getComponent();
        // The five picker intents of ADR-0057 go to the system picker, which the app's manifest does not handle.
        final int pickerKind = component == null ? PickerIntents.kindOf(intent) : PickerIntents.NONE;
        // The links, mail, calls, texts, maps and shares of ADR-0062 go to the activity that asks iOS for them.
        final boolean open = component == null && pickerKind == PickerIntents.NONE && OpenIntents.kindOf(intent) != 0;
        // Activity.requestPermissions' intent goes to the activity that stands in for the permission dialog (ADR-0063).
        final boolean permissions = component == null && pickerKind == PickerIntents.NONE && !open
                && PackageManager.ACTION_REQUEST_PERMISSIONS.equals(intent.getAction());
        ActivityInfo info = pickerKind != PickerIntents.NONE
                ? PickerIntents.activityInfo(mPackage.applicationInfo(0), pickerKind)
                : open ? OpenIntents.activityInfo(mPackage.applicationInfo(0))
                : permissions ? SystemPermissionActivity.activityInfo(mPackage.applicationInfo(0))
                : component != null
                ? mPackage.activityInfo(component, PackageManager.GET_SHARED_LIBRARY_FILES, false) : null;
        final int result = component == null && pickerKind == PickerIntents.NONE && !open && !permissions
                ? ActivityManager.START_INTENT_NOT_RESOLVED
                : info == null ? ActivityManager.START_CLASS_NOT_FOUND : ActivityManager.START_SUCCESS;
        if (info != null) {
            mPendingStarts.incrementAndGet();
        } else if (source == null || (requestCode < 0 && !forward)) {
            return result;
        }
        final Intent started = new Intent(intent);
        if (pickerKind != PickerIntents.NONE || open || permissions) {
            started.setComponent(new ComponentName(info.packageName, info.name));
        }
        sMainThreadHandler.post(() -> addTransaction(
                () -> handleStartTransaction(started, info, source, resultWho, requestCode, null)));
        return result;
    }

    /**
     * Activity.finish's, through LocalActivityClientController, from any thread: true once the finish is asked for;
     * false for an unknown activity and, without a session, for the last one, which then stays, as before. Its result
     * goes to the activity that waits for it. As ActivityClientController.finishActivity, FINISH_TASK_WITH_ACTIVITY,
     * and finishAndRemoveTask's FINISH_TASK_WITH_ROOT_ACTIVITY from the task's root, end the whole task, bottom up as
     * Task.removeActivities goes; from any other activity finishAndRemoveTask finishes it alone (step b3.5b2-2a).
     */
    boolean finishActivity(IBinder token, int resultCode, Intent resultData, int finishTask) {
        final ActivityClientRecord r = mActivities.get(token);
        if (r == null || (mSessionEvents == null && mTask.size() == 1 && mPendingStarts.get() == 0)) {
            return false;
        }
        final boolean wholeTask = finishTask == Activity.FINISH_TASK_WITH_ACTIVITY
                || (finishTask == Activity.FINISH_TASK_WITH_ROOT_ACTIVITY && isTaskRoot(token));
        final Intent data = resultData != null ? new Intent(resultData) : null;
        sMainThreadHandler.post(() -> addTransaction(() -> {
            if (wholeTask) {
                finishRecords(new ArrayList<>(mTask), Activity.RESULT_CANCELED, null);
            } else {
                finishRecords(Collections.singletonList(r), resultCode, data);
            }
        }));
        return true;
    }

    /**
     * Activity.finishActivity(requestCode)'s, as ActivityRecord.finishIfSubActivity: the activities that `token`'s
     * started for that request finish, and each returns RESULT_CANCELED to it.
     */
    void finishSubActivity(IBinder token, String resultWho, int requestCode) {
        final ActivityClientRecord caller = mActivities.get(token);
        if (caller == null) {
            return;
        }
        sMainThreadHandler.post(() -> addTransaction(() -> {
            ArrayList<ActivityClientRecord> started = new ArrayList<>();
            for (ActivityClientRecord r : reversedTask()) {
                if (r.resultTo == caller && r.requestCode == requestCode && Objects.equals(r.resultWho, resultWho)) {
                    started.add(r);
                }
            }
            finishRecords(started, Activity.RESULT_CANCELED, null);
        }));
    }

    /**
     * Activity.finishAffinity's, as ActivityRecord.finishIfSameAffinity over the task: the activity and those below it,
     * top down, until one has another affinity; each returns RESULT_CANCELED.
     */
    boolean finishActivityAffinity(IBinder token) {
        final ActivityClientRecord r = mActivities.get(token);
        if (r == null) {
            return false;
        }
        sMainThreadHandler.post(() -> addTransaction(() -> {
            ArrayList<ActivityClientRecord> same = new ArrayList<>();
            for (int i = mTask.indexOf(r); i >= 0; i--) {
                ActivityClientRecord below = mTask.get(i);
                if (!Objects.equals(below.activityInfo.taskAffinity, r.activityInfo.taskAffinity)) {
                    break;
                }
                same.add(below);
            }
            finishRecords(same, Activity.RESULT_CANCELED, null);
        }));
        return true;
    }

    /** ActivityClientController.getCallingActivity's: the activity that waits for this one's result, or null. */
    ComponentName callingActivity(IBinder token) {
        final ActivityClientRecord r = mActivities.get(token);
        final ActivityClientRecord caller = r != null ? r.resultTo : null;
        return caller != null ? caller.intent.getComponent() : null;
    }

    /** ActivityClientController.getCallingPackage's: that activity's package, or null. */
    String callingPackage(IBinder token) {
        final ActivityClientRecord r = mActivities.get(token);
        final ActivityClientRecord caller = r != null ? r.resultTo : null;
        return caller != null ? caller.activityInfo.packageName : null;
    }

    /**
     * Activity.navigateUpTo's, through LocalActivityClientController, as Task.navigateUpTo (step b3.5b2-2b): the answer
     * at once, whether the destination is in the task below the activity and of the manifest, as the start that
     * follows succeeds then; the finishes and the start as a transaction.
     */
    boolean navigateUpTo(IBinder token, Intent destIntent, int resultCode, Intent resultData) {
        final ActivityClientRecord srec = mActivities.get(token);
        if (srec == null || !mTask.contains(srec)) {
            return false;
        }
        final ComponentName dest = destIntent.getComponent();
        final ActivityInfo info = upParent(srec, dest) != null
                ? mPackage.activityInfo(dest, PackageManager.GET_SHARED_LIBRARY_FILES, false) : null;
        if (info != null) {
            mPendingStarts.incrementAndGet();
        }
        final Intent up = new Intent(destIntent);
        final Intent data = resultData != null ? new Intent(resultData) : null;
        sMainThreadHandler.post(() -> addTransaction(
                () -> handleNavigateUpTransaction(srec, up, info, resultCode, data)));
        return info != null;
    }

    /**
     * ActivityClientController.shouldUpRecreateTask's, as Task.shouldUpRecreateTaskLocked: whether the destination's
     * affinity is not the task's, which its root's is in the one task; there are no document tasks.
     */
    boolean shouldUpRecreateTask(IBinder token, String destAffinity) {
        if (mActivities.get(token) == null) {
            return false;
        }
        final String affinity = mTask.isEmpty() ? null : mTask.get(0).activityInfo.taskAffinity;
        return affinity == null || !affinity.equals(destAffinity);
    }

    /** As Task.navigateUpTo's search: the top activity below `srec` with the destination's package and class, or null. */
    private ActivityClientRecord upParent(ActivityClientRecord srec, ComponentName dest) {
        if (dest == null) {
            return null;
        }
        for (int i = mTask.indexOf(srec) - 1; i >= 0; i--) {
            final ActivityInfo below = mTask.get(i).activityInfo;
            if (below.packageName.equals(dest.getPackageName()) && below.name.equals(dest.getClassName())) {
                return mTask.get(i);
            }
        }
        return null;
    }

    /**
     * As Task.navigateUpTo: the activities from `srec` down to the parent finish, top down, the first with the result
     * and the others with RESULT_CANCELED; then the destination starts as from the parent, which waits for its
     * result with navigateUpTo's request code 0, and a new instance has the parent finish before its launch, as
     * START_SUCCESS has it finish once the start is made. Without the parent in the task, `srec` alone finishes.
     */
    private void handleNavigateUpTransaction(ActivityClientRecord srec, Intent destIntent, ActivityInfo info,
            int resultCode, Intent resultData) {
        final ActivityClientRecord parent = mTask.contains(srec) ? upParent(srec, destIntent.getComponent()) : null;
        if (parent == null || info == null) {
            if (info != null) {
                mPendingStarts.decrementAndGet();
            }
            finishRecords(Collections.singletonList(srec), resultCode, resultData);
            return;
        }
        final ArrayList<ActivityClientRecord> above = new ArrayList<>();
        for (int i = mTask.indexOf(srec); mTask.get(i) != parent; i--) {
            above.add(mTask.get(i));
        }
        finishEach(above.subList(0, 1), resultCode, resultData, true);
        finishEach(above.subList(1, above.size()), Activity.RESULT_CANCELED, null, true);
        handleStartTransaction(destIntent, info, parent, null, 0,
                () -> finishEach(Collections.singletonList(parent), Activity.RESULT_CANCELED, null, true));
    }

    private void addTransaction(Runnable transaction) {
        mTransactions.add(transaction);
        runTransactions();
    }

    /** Runs the transactions asked for, in their order, while the session has the app resumed. */
    private void runTransactions() {
        while (mSessionResumed && !mTransactions.isEmpty()) {
            mTransactions.poll().run();
        }
    }

    /**
     * As Android's start of an activity in the task (ActivityStarter.startActivityInner). First who gets its result, as
     * executeRequest decides it: a start that failed returns RESULT_CANCELED at once, and so does one into a new task,
     * as setInitialState's sendNewTaskResultRequestIfNeeded has it (step b3.5b2-2a). Then the launch mode's
     * FLAG_ACTIVITY_NEW_TASK, as computeLaunchingTaskFlags adds it to the intent, and the task's flags, as
     * complyActivityFlags applies them (step b3.5b2-2b): NEW_TASK with CLEAR_TASK finishes the task's activities,
     * bottom up, as removeActivities goes; CLEAR_TOP, singleTask and singleInstance(PerTask) finish those above the
     * target's newest instance, top down, and a standard target without SINGLE_TOP too; REORDER_TO_FRONT moves that
     * instance to the top. Then, as deliverToCurrentTopIfNeeded, a singleTop target or SINGLE_TOP finds the top one of
     * its class, whose start's result is cancelled. Such an activity gets the intent and comes to the top: the resumed
     * one pauses, the user leaving it unless the intent has FLAG_ACTIVITY_NO_USER_ACTION, and it resumes. Otherwise
     * the resumed one pauses so, `beforeLaunch` runs, and the new one's launch goes up to its resume, the top resumed
     * state and the session's window. One that finished in onCreate goes without them.
     */
    private void handleStartTransaction(Intent intent, ActivityInfo info, ActivityClientRecord source,
            String resultWho, int requestCode, Runnable beforeLaunch) {
        ActivityClientRecord resultRecord = null;
        if (source != null && mTask.contains(source)) {
            if (requestCode >= 0 && !source.finishRequested) {
                resultRecord = source;
            }
            if ((intent.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0) {
                resultRecord = source.resultTo != null && mTask.contains(source.resultTo) ? source.resultTo : null;
                resultWho = source.resultWho;
                requestCode = source.requestCode;
                source.resultTo = null;
            }
        }
        if (info == null) {
            if (resultRecord != null) {
                sendResult(resultRecord, resultWho, requestCode, Activity.RESULT_CANCELED, null);
            }
            return;
        }
        mPendingStarts.decrementAndGet();
        final int mode = info.launchMode;
        if (mode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK) {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        }
        if (resultRecord != null && (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0) {
            sendResult(resultRecord, resultWho, requestCode, Activity.RESULT_CANCELED, null);
            resultRecord = null;
        }
        if (mode == ActivityInfo.LAUNCH_SINGLE_TASK || mode == ActivityInfo.LAUNCH_SINGLE_INSTANCE
                || (source != null && source.activityInfo.launchMode == ActivityInfo.LAUNCH_SINGLE_INSTANCE)) {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        }
        final int flags = intent.getFlags();
        final boolean userLeaving = (flags & Intent.FLAG_ACTIVITY_NO_USER_ACTION) == 0;
        final ComponentName component = componentOf(info);
        ActivityClientRecord target = null;
        if ((flags & (Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK))
                == (Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK)) {
            finishEach(new ArrayList<>(mTask), Activity.RESULT_CANCELED, null, true);
        } else if ((flags & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0 || mode == ActivityInfo.LAUNCH_SINGLE_TASK
                || mode == ActivityInfo.LAUNCH_SINGLE_INSTANCE || mode == ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK) {
            final ActivityClientRecord found = findInTask(component);
            if (found != null) {
                final ArrayList<ActivityClientRecord> above =
                        new ArrayList<>(mTask.subList(mTask.indexOf(found) + 1, mTask.size()));
                Collections.reverse(above);
                finishEach(above, Activity.RESULT_CANCELED, null, true);
                if (found.activityInfo.launchMode == ActivityInfo.LAUNCH_MULTIPLE
                        && (flags & Intent.FLAG_ACTIVITY_SINGLE_TOP) == 0) {
                    finishEach(Collections.singletonList(found), Activity.RESULT_CANCELED, null, true);
                } else {
                    target = found;
                }
            }
        } else if ((flags & Intent.FLAG_ACTIVITY_REORDER_TO_FRONT) != 0) {
            target = findInTask(component);
            if (target != null) {
                mTask.remove(target);
                mTask.add(target);
            }
        }
        if (target == null) {
            final ActivityClientRecord top = topLiveRecord();
            if (top != null && component.equals(componentOf(top.activityInfo))
                    && ((flags & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0 || mode == ActivityInfo.LAUNCH_SINGLE_TOP)) {
                if (resultRecord != null) {
                    sendResult(resultRecord, resultWho, requestCode, Activity.RESULT_CANCELED, null);
                }
                target = top;
            }
        }
        final ActivityClientRecord resumed = resumedRecord();
        if (target != null) {
            deliverNewIntent(target, intent);
            if (target.state != ON_RESUME) {
                if (resumed != null && resumed != target) {
                    handleTopResumedActivityChanged(resumed, false);
                    handlePauseActivity(resumed, userLeaving);
                }
                resumeRecord(target);
            }
            sendResultsToResumed();
            scheduleIdle();
            return;
        }
        if (resumed != null) {
            handleTopResumedActivityChanged(resumed, false);
            handlePauseActivity(resumed, userLeaving);
        }
        if (beforeLaunch != null) {
            beforeLaunch.run();
        }
        ActivityClientRecord r = newRecord(intent, info);
        r.resultTo = resultRecord;
        r.resultWho = resultWho;
        r.requestCode = requestCode;
        if (performLaunchActivity(r) == null) {
            removeRecord(r);
            ActivityClientRecord next = topLiveRecord();
            if (next != null) {
                resumeRecord(next);
            }
        } else if (!finishing(r)) {
            handleStartActivity(r);
            handleResumeActivity(r);
            handleTopResumedActivityChanged(r, true);
            updateWindows();
        }
        sendResultsToResumed();
        scheduleIdle();
    }

    /** As ActivityRecord.mActivityComponent: an alias stands for its target activity. */
    private static ComponentName componentOf(ActivityInfo info) {
        return new ComponentName(info.packageName, info.targetActivity != null ? info.targetActivity : info.name);
    }

    /** As Task.findActivityInHistory: the task's newest activity of the component that is not finishing, or null. */
    private ActivityClientRecord findInTask(ComponentName component) {
        for (int i = mTask.size() - 1; i >= 0; i--) {
            final ActivityClientRecord r = mTask.get(i);
            if (!finishing(r) && component.equals(componentOf(r.activityInfo))) {
                return r;
            }
        }
        return null;
    }

    /** The task's resumed activity, or null. */
    private ActivityClientRecord resumedRecord() {
        for (int i = mTask.size() - 1; i >= 0; i--) {
            if (mTask.get(i).state == ON_RESUME) {
                return mTask.get(i);
            }
        }
        return null;
    }

    /**
     * As ActivityRecord.deliverNewIntentLocked (step b3.5b2-2b): a resumed activity gets the intent at once, paused for
     * it and resumed again, as NewIntentItem with its resume has it; a paused one at once and stays paused; any other
     * at its next resume, after its results. The intent is a copy, as Binder's parcel gives the activity one.
     */
    private void deliverNewIntent(ActivityClientRecord r, Intent intent) {
        final Intent copy = new Intent(intent);
        if (r.state == ON_RESUME) {
            handlePauseActivity(r, false);
            deliverNewIntents(r, Collections.singletonList(copy));
            handleResumeActivity(r);
        } else if (r.state == ON_PAUSE) {
            deliverNewIntents(r, Collections.singletonList(copy));
        } else {
            if (r.pendingIntents == null) {
                r.pendingIntents = new ArrayList<>();
            }
            r.pendingIntents.add(copy);
        }
    }

    /**
     * As AOSP's deliverNewIntents: each intent with the activity's class loader, then onNewIntent through
     * Instrumentation. Minato's activities have no referrer (performLaunchActivity's), so ReferrerIntent's is not set.
     */
    private void deliverNewIntents(ActivityClientRecord r, List<Intent> intents) {
        for (Intent intent : intents) {
            intent.setExtrasClassLoader(r.activity.getClassLoader());
            intent.prepareToEnterProcess(false, null);
            r.activity.mFragments.noteStateNotSaved();
            mInstrumentation.callActivityOnNewIntent(r.activity, intent);
        }
    }

    /**
     * As Android's finishIfPossible for each of `records` in their order (step b3.5b2-2a), as finishEach has it; then
     * the activity below the top comes back, unless a start asked for is still to run, which then takes the top, and
     * the results for resumed activities go. The task's last finish asks the host, which answers with the quit
     * (ADR-0043 step c1).
     */
    private void finishRecords(List<ActivityClientRecord> records, int resultCode, Intent resultData) {
        final boolean topFinished = finishEach(records, resultCode, resultData, false);
        if (topLiveRecord() == null && mPendingStarts.get() == 0) {
            mResultsToResumed.clear();
            requestSessionFinish();
            return;
        }
        if (topFinished) {
            ActivityClientRecord next = topLiveRecord();
            if (next != null) {
                resumeRecord(next);
            }
        }
        sendResultsToResumed();
        scheduleIdle();
    }

    /**
     * As finishIfPossible for each of `records` in their order, without what comes after it. Each sends its result to
     * the activity that waits for it, which gets it at its next resume or, when that one is resumed, at the
     * transaction's end, as finishActivityResults posts it. The top one, when resumed, is finishing from then on, as
     * PauseActivityItem's `finished` makes it, and its top resumed state goes and it pauses when an activity is left
     * or a start comes after it (`startFollows`, step b3.5b2-2b). One that had stopped goes at once, as Android destroys
     * an activity that is no longer visible; a paused one stops and goes once the main thread is idle. Returns
     * whether the top one finished.
     */
    private boolean finishEach(List<ActivityClientRecord> records, int resultCode, Intent resultData,
            boolean startFollows) {
        final ActivityClientRecord top = topRecord();
        boolean topFinished = false;
        for (ActivityClientRecord r : records) {
            if (!mTask.contains(r) || r.finishRequested) {
                continue;
            }
            r.finishRequested = true;
            final ActivityClientRecord to = r.resultTo;
            r.resultTo = null;
            if (to != null && mTask.contains(to)) {
                final String who = r.resultWho;
                final int code = r.requestCode;
                if (to.state == ON_RESUME) {
                    mResultsToResumed.add(() -> sendResult(to, who, code, resultCode, resultData));
                } else {
                    sendResult(to, who, code, resultCode, resultData);
                }
            }
            if (r == top) {
                topFinished = true;
                if (r.state == ON_RESUME) {
                    r.activity.mFinished = true;
                    if (startFollows || topLiveRecord() != null || mPendingStarts.get() > 0) {
                        handleTopResumedActivityChanged(r, false);
                        handlePauseActivity(r, false);
                    }
                }
            } else if (r.state == ON_STOP) {
                handleDestroyActivity(r);
            }
        }
        return topFinished;
    }

    /** The results that waited for the transaction's end, in their order. */
    private void sendResultsToResumed() {
        while (!mResultsToResumed.isEmpty()) {
            mResultsToResumed.remove(0).run();
        }
    }

    /**
     * As ActivityRecord.sendResult: a resumed activity gets the result at once, paused for it and resumed again, as
     * ActivityResultItem has it after S (CALL_ACTIVITY_RESULT_BEFORE_RESUME); any other at its next resume. The data
     * is a copy, as Binder's parcel gives the activity one.
     */
    private void sendResult(ActivityClientRecord r, String who, int requestCode, int resultCode, Intent data) {
        if (!mTask.contains(r)) {
            return;
        }
        final Result result = new Result(who, requestCode, resultCode, data != null ? new Intent(data) : null);
        if (r.state == ON_RESUME) {
            handlePauseActivity(r, false);
            deliverResults(r, Collections.singletonList(result));
            handleResumeActivity(r);
        } else {
            if (r.pendingResults == null) {
                r.pendingResults = new ArrayList<>();
            }
            r.pendingResults.add(result);
        }
    }

    /**
     * What waited for the activity's resume, before its onResume: its results, then its new intents, as
     * resumeTopActivity schedules ActivityResultItem before NewIntentItem.
     */
    private void deliverPending(ActivityClientRecord r) {
        if (r.pendingResults != null) {
            final ArrayList<Result> results = r.pendingResults;
            r.pendingResults = null;
            deliverResults(r, results);
        }
        if (r.pendingIntents != null) {
            final ArrayList<Intent> intents = r.pendingIntents;
            r.pendingIntents = null;
            deliverNewIntents(r, intents);
        }
    }

    /** As AOSP's deliverResults: each result's data with the activity's class loader, then dispatchActivityResult. */
    private void deliverResults(ActivityClientRecord r, List<Result> results) {
        for (Result result : results) {
            if (result.data != null) {
                result.data.setExtrasClassLoader(r.activity.getClassLoader());
                result.data.prepareToEnterProcess(false, null);
            }
            r.activity.dispatchActivityResult(result.who, result.requestCode, result.resultCode, result.data,
                    "sendResult");
        }
    }

    /**
     * As Android's resume of an activity that comes back to the top: onRestart and onStart after its stop, the results
     * (step b3.5b2-2a) and new intents (step b3.5b2-2b) that waited for it, onResume, the top resumed state and the
     * session's window.
     */
    private void resumeRecord(ActivityClientRecord r) {
        if (r.state == ON_STOP) {
            performRestartActivity(r);
        } else if (r.state == ON_CREATE) {
            handleStartActivity(r);
        }
        if (r.state == ON_START || r.state == ON_PAUSE) {
            deliverPending(r);
            handleResumeActivity(r);
            handleTopResumedActivityChanged(r, true);
            updateWindows();
        }
    }

    /** The last activity's finish: with a session, the host answers it with the quit (ADR-0043 step c1). */
    private void requestSessionFinish() {
        if (mSessionEvents == null) {
            return;
        }
        int error = nativeSessionRequest(REQUEST_FINISH, 0, 0);
        if (error != 0) {
            Log.e(TAG, "The session refused FINISH: " + error);
        }
    }

    /**
     * As Android's activityIdle, once the main thread has nothing left to do: the activities below the live top stop,
     * and the finished ones go. A finished one that is still paused stops first with its state saved unless the app
     * finished it, as DestroyActivityItem passes ON_STOP; a no-history one finishes instead of stopping, as
     * stopIfPossible has it, its result RESULT_CANCELED (step b3.5b2-2b). While transactions wait for the session,
     * their own pass follows them.
     */
    private void scheduleIdle() {
        if (mIdleScheduled) {
            return;
        }
        mIdleScheduled = true;
        Looper.myQueue().addIdleHandler(() -> {
            mIdleScheduled = false;
            if (mTransactions.isEmpty()) {
                ActivityClientRecord top = topLiveRecord();
                // As ensureActivitiesVisible: an activity stops when one that occludes it covers it, so the ones below
                // a translucent top, a dialog theme or the photo picker, stay paused (ADR-0057 step a), and one that
                // setTranslucent shows again starts again (ADR-0041 gap 4 (b3)); ActivityWindows' pass says which.
                updateWindows();
                for (ActivityClientRecord r : reversedTask()) {
                    if (r == top || !mTask.contains(r)) {
                        continue;
                    }
                    final boolean hidden = r.windows.mode() == ActivityWindows.HIDDEN;
                    if (!finishing(r) && hidden && r.state == ON_PAUSE && noHistory(r)) {
                        finishEach(Collections.singletonList(r), Activity.RESULT_CANCELED, null, false);
                    }
                    if (finishing(r)) {
                        if (r.state == ON_PAUSE && !r.activity.mFinished) {
                            handleStopActivity(r);
                        }
                        handleDestroyActivity(r);
                    } else if (hidden && (r.state == ON_PAUSE || r.state == ON_START)) {
                        handleStopActivity(r);
                    } else if (!hidden && r.state == ON_STOP && mSessionResumed) {
                        // As makeVisibleIfNeeded's StartActivityItem: onRestart and onStart, and the window shown;
                        // in the foreground only, as the background stopped the whole task.
                        performRestartActivity(r);
                        updateVisibility(r, true);
                    }
                }
                for (ActivityClientRecord r : new ArrayList<>(mTask)) {
                    if (r.conversionPending) {
                        r.conversionPending = false;
                        r.activity.onTranslucentConversionComplete(true);
                    }
                }
                sendResultsToResumed();
            }
            return false;
        });
    }

    /** Whether the activity's theme has the window attribute, as ActivityInfo.isTranslucentOrFloating reads it. */
    private static boolean windowStyle(ActivityClientRecord r, int attribute) {
        if (r.activity == null || r.activity.getWindow() == null) {
            return false;
        }
        final TypedArray style = r.activity.getWindow().getWindowStyle();
        return style != null && style.getBoolean(attribute, false);
    }

    /** The activity's slot for ActivityWindows' pass, from its record. */
    private static void describe(ActivityClientRecord r) {
        r.windows.finishing = finishing(r);
        r.windows.translucentTheme = windowStyle(r, com.android.internal.R.styleable.Window_windowIsTranslucent);
        r.windows.floatingTheme = windowStyle(r, com.android.internal.R.styleable.Window_windowIsFloating);
    }

    /**
     * ADR-0041 gap 4 (b3), the plan's decision 3, as Android 15's ensureActivitiesVisible gives each activity its
     * window: ActivityWindows' pass over the task, then, for each activity whose window changed, its windows taken
     * away, top down; the session's window and input to the lowest visible activity; and each visible one's windows
     * again, bottom up, its own as a requested one above the lowest or for a floating one. An activity whose own window
     * came with its resume after its create gets its windows then. The lifecycle follows in the idle pass.
     */
    private void updateWindows() {
        final ArrayList<ActivityWindows.Slot> slots = new ArrayList<>();
        for (ActivityClientRecord r : mTask) {
            describe(r);
            slots.add(r.windows);
        }
        final List<ActivityWindows.Slot> changed = mWindows.update(slots);
        final ArrayList<ActivityClientRecord> again = new ArrayList<>();
        for (ActivityClientRecord r : mTask) {
            if (changed.contains(r.windows)
                    || (!r.windowsApplied && r.windows.mode() != ActivityWindows.HIDDEN && rootOf(r) != null)) {
                again.add(r);
            }
        }
        final WindowManagerGlobal windows = WindowManagerGlobal.getInstance();
        for (int i = again.size() - 1; i >= 0; i--) {
            windows.withdrawActivityWindows(again.get(i).token);
            again.get(i).windowsApplied = false;
        }
        // The one that gets the session's window is on it before the move, so that its SurfaceViews ask c4's way.
        final ActivityClientRecord session = recordOf(mWindows.session());
        if (session != null && again.contains(session)) {
            windows.restoreActivityWindows(session.token, false, session.windows.key());
            session.windowsApplied = rootOf(session) != null;
        }
        moveHostTo(session, recordOf(mWindows.input()));
        for (ActivityClientRecord r : again) {
            if (r != session && r.windows.mode() != ActivityWindows.HIDDEN) {
                windows.restoreActivityWindows(r.token, r.windows.mode() == ActivityWindows.REQUESTED,
                        r.windows.key());
                r.windowsApplied = rootOf(r) != null;
            }
        }
    }

    private ActivityClientRecord recordOf(ActivityWindows.Slot slot) {
        for (ActivityClientRecord r : mTask) {
            if (r.windows == slot) {
                return r;
            }
        }
        return null;
    }

    /**
     * @hide WindowManagerGlobal's, at an application window's add (gap 4 (b3)): the group of the activity's windows,
     * its key, or -1 while it is not visible, when the window waits for it.
     */
    public int windowGroupOf(IBinder token) {
        final ActivityClientRecord r = token != null ? mActivities.get(token) : null;
        return r == null || r.windows.mode() == ActivityWindows.HIDDEN ? -1 : r.windows.key();
    }

    /**
     * LocalActivityClientController's convertFromTranslucent (`occludes`) and convertToTranslucent, as
     * ActivityRecord.setOccludesParent: whether the activity occludes the ones below it changed, for which the pass
     * runs once the main thread is idle: the ones below stop or start again. A conversion to translucent then hears
     * onTranslucentConversionComplete, as once the activities below drew.
     */
    boolean setOccludesParent(IBinder token, boolean occludes) {
        final ActivityClientRecord r = token != null ? mActivities.get(token) : null;
        if (r == null || r.activity == null) {
            return false;
        }
        describe(r);
        final boolean before = r.windows.occludes();
        r.windows.converted = !occludes;
        if (before == occludes) {
            return false;
        }
        r.conversionPending = !occludes;
        scheduleIdle();
        return true;
    }

    /** As ActivityRecord.isNoHistory: FLAG_ACTIVITY_NO_HISTORY in its intent or noHistory in its manifest. */
    private static boolean noHistory(ActivityClientRecord r) {
        return (r.intent.getFlags() & Intent.FLAG_ACTIVITY_NO_HISTORY) != 0
                || (r.activityInfo.flags & ActivityInfo.FLAG_NO_HISTORY) != 0;
    }

    /**
     * The session's window goes to `windowRecord`'s window, and its input channel and focus to `inputRecord`'s: the
     * lowest visible activity's (step b3.5b2-1, ADR-0041 gap 4 (b3)), as Android's window manager gives them. The
     * window before loses its surface, so its SurfaceViews' surfaces go; the new one gets the window, which its next
     * traversal draws whole. Below a floating activity at the bottom (no `windowRecord`) the session's window is empty.
     * The screen keeps the old window's last frame until the new one's first. An activity whose own window is not
     * added yet keeps nothing from the window before.
     */
    private void moveHostTo(ActivityClientRecord windowRecord, ActivityClientRecord inputRecord) {
        final ViewRootImpl window = windowRecord != null ? rootOf(windowRecord) : null;
        final ViewRootImpl from = mWindowRoot;
        if (window != from && (window != null || windowRecord == null)) {
            mWindowRoot = window;
            if (from != null) {
                from.setHostWindow(0, 0, 0);
            }
            if (window != null && mHostWindow != 0) {
                window.setHostWindow(mHostWindow, mHostWidth, mHostHeight);
            } else if (window == null && mHostWindow != 0 && inputRecord != null) {
                clearSessionWindow();
            }
        }
        moveInputTo(inputRecord != null ? rootOf(inputRecord) : null);
    }

    /**
     * The session's input channel and focus go to `to`. The window before loses its focus, its open touch gesture with
     * a CANCEL (step b3.5b2-1b) and its share of the channel; the new one gets a share, whose queue the host connects
     * once, with touches from their next DOWN, and the focus, which it takes once it drew.
     */
    private void moveInputTo(ViewRootImpl to) {
        final ViewRootImpl from = mHostRoot;
        if (to == null || to == from) {
            return;
        }
        mHostRoot = to;
        InputChannel input = mHostInput;
        if (from != null && input != null) {
            WindowManagerGlobal.getInstance().cancelRoutedTouch();
            // A finger that is down stays the old window's: its gesture ends there with a CANCEL, and the new
            // window gets touches from their next DOWN (step b3.5b2-1b).
            from.cancelTouch();
            input = input.dup();
            from.setInputChannel(null);
        }
        mHostInput = input;
        if (input != null && from != null) {
            to.takeInputChannelAtSwitch(input);
        } else if (input != null) {
            to.setInputChannel(input);
        }
        // The focus leaves the window before and goes to the new one's top focusable window (ADR-0041 gap 4 (b2)).
        WindowManagerGlobal.getInstance().setSessionWindow(to, mHostFocus);
    }

    /**
     * The empty session's window below a floating activity at the bottom (gap 4 (b3)): black, where Android shows the
     * home screen.
     */
    @SuppressWarnings("deprecation") // Surface.transferFrom, as our ViewRootImpl takes its window's surface.
    private void clearSessionWindow() {
        final Surface surface = new Surface();
        try {
            surface.transferFrom(WindowSurface.create(mHostWindow, mHostWidth, mHostHeight));
            final Canvas canvas = surface.lockCanvas(null);
            canvas.drawColor(Color.BLACK);
            surface.unlockCanvasAndPost(canvas);
        } catch (RuntimeException e) {
            Log.w(TAG, "The empty session's window was not cleared", e);
        } finally {
            surface.release();
        }
    }

    /**
     * The activity's window once its decor is added: AOSP's setView makes the root the decor's parent at once, while
     * the decor's getViewRootImpl answers only after the first traversal attached it.
     */
    private static ViewRootImpl rootOf(ActivityClientRecord r) {
        View decor = r.window != null ? r.window.peekDecorView() : null;
        ViewParent parent = decor != null ? decor.getParent() : null;
        return parent instanceof ViewRootImpl ? (ViewRootImpl) parent : null;
    }

    /** The activity whose orientation the display follows: the task's top. */
    ActivityClientRecord topRecordForOrientation() {
        return topRecord();
    }

    private ActivityClientRecord topRecord() {
        return mTask.isEmpty() ? null : mTask.get(mTask.size() - 1);
    }

    /** The task's activities from the top down, as a copy that their destroys do not change. */
    private ArrayList<ActivityClientRecord> reversedTask() {
        ArrayList<ActivityClientRecord> records = new ArrayList<>(mTask);
        java.util.Collections.reverse(records);
        return records;
    }

    /** The task's top activity that is not finishing, or null. */
    private ActivityClientRecord topLiveRecord() {
        for (int i = mTask.size() - 1; i >= 0; i--) {
            if (!finishing(mTask.get(i))) {
                return mTask.get(i);
            }
        }
        return null;
    }

    /** Whether the activity is finishing: the app finished it, or the system did (finishRequested). */
    private static boolean finishing(ActivityClientRecord r) {
        return r.activity == null || r.activity.mFinished || r.finishRequested;
    }

    /**
     * As Android's task root for getTaskForActivity with onlyRoot: the activity at or below the task's first one that
     * is not finishing, or the bottom one while all are.
     */
    boolean isTaskRoot(IBinder token) {
        int index = -1;
        int root = 0;
        for (int i = mTask.size() - 1; i >= 0; i--) {
            ActivityClientRecord r = mTask.get(i);
            if (r.token == token) {
                index = i;
            }
            if (!finishing(r)) {
                root = i;
            }
        }
        return index >= 0 && index <= root;
    }

    /**
     * As Android 15's ActivityClientController.shouldMoveTaskToBack, for Minato's one task, which the host starts from
     * its library as Android's launcher does (LAUNCH_SOURCE_TYPE_HOME): whether a back moves the task to the back
     * rather than finishing the activity. It does for the task's root that is its top, when it is the activity the task
     * was made for, started with a main intent (ActivityRecord.isMainIntent).
     */
    boolean backMovesTaskToBack(IBinder token) {
        ActivityClientRecord r = mActivities.get(token);
        if (r == null || r.intent == null || !isTaskRoot(token) || !isTopOfTask(token)
                || mTaskActivity == null || !mTaskActivity.equals(r.intent.getComponent())) {
            return false;
        }
        Intent intent = r.intent;
        return Intent.ACTION_MAIN.equals(intent.getAction()) && intent.getCategories() != null
                && intent.getCategories().size() == 1 && intent.hasCategory(Intent.CATEGORY_LAUNCHER)
                && intent.getData() == null && intent.getType() == null;
    }

    /**
     * The task's move to the back (ADR-0041 decision 10): with a session, the host opens its menu instead, where
     * Android leaves a warm task behind and Minato's would end; whether the request went. Without a session nothing
     * moves.
     */
    boolean moveTaskToBack() {
        if (mSessionEvents == null) {
            return false;
        }
        int error = nativeSessionRequest(REQUEST_BACK_AT_ROOT, 0, 0);
        if (error != 0) {
            Log.e(TAG, "The session refused BACK_AT_ROOT: " + error);
        }
        return error == 0;
    }

    /** As Android's isTopOfTask: whether the activity is the task's top one that is not finishing. */
    boolean isTopOfTask(IBinder token) {
        ActivityClientRecord top = topLiveRecord();
        return top != null && top.token == token;
    }

    /**
     * @hide ViewRootImpl's, at the first draw of an activity's window on the host's window: once, the line that
     * Android 15's ActivityMetricsLogger.logAppDisplayed logs for a launch that made the activity, a cold or a warm
     * one, with the time since its start. An activity brought back, a hot launch, logs none, as on Android. The
     * corpus harness reads it as the app's start.
     */
    public static void reportWindowsDrawn(IBinder token) {
        final ActivityThread thread = sCurrentActivityThread;
        final ActivityClientRecord r = thread != null ? thread.mActivities.get(token) : null;
        if (r == null || r.launchStartTime == 0) {
            return;
        }
        StringBuilder line = new StringBuilder("Displayed ");
        line.append(r.intent.getComponent().flattenToShortString()).append(" for user ").append(UserHandle.myUserId())
                .append(": ");
        TimeUtils.formatDuration(SystemClock.elapsedRealtime() - r.launchStartTime, line);
        Log.i("ActivityTaskManager", line.toString());
        r.launchStartTime = 0;
    }

    /**
     * @hide ViewRootImpl's (ADR-0043 c3): the window's format, a PixelFormat, to the session's host, as
     * ANativeActivity_setWindowFormat sends it; the runtime reports its opacity. Nothing without a session.
     */
    /**
     * @hide SurfaceView's (ADR-0043 c4): a surface of the host's at a frame in the window's pixels, below the window
     * (z < 0) or above it, with its buffer's size, 0 x 0 for the frame's, and format. The session answers with
     * SURFACE_CREATED; a negative answer is an errno, -EOPNOTSUPP without a session.
     */
    public static int makeSurface(long token, int z, int left, int top, int right, int bottom, int width, int height,
            int format) {
        return requestSurface(SURFACE_MAKE, z, token, left, top, right, bottom, width, height, format);
    }

    /** @hide SurfaceView's: a new frame, order or size; a new size is answered with SURFACE_RESIZED. */
    public static int placeSurface(long token, int z, int left, int top, int right, int bottom, int width,
            int height) {
        return requestSurface(SURFACE_PLACE, z, token, left, top, right, bottom, width, height, 0);
    }

    /** @hide SurfaceView's: the surface goes, after its surfaceDestroyed; no answer. */
    public static int removeSurface(long token) {
        return requestSurface(SURFACE_REMOVE, 0, token, 0, 0, 0, 0, 0, 0, 0);
    }

    private static int requestSurface(int op, int z, long token, int left, int top, int right, int bottom, int width,
            int height, int format) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestSurface(op, z, token, left, top, right, bottom, width, height, format);
    }

    /**
     * @hide ViewRootImpl's (ADR-0041 gap 4 (b)): a requested window of `type` at a frame in the session's pixels, a
     * sub-window of `parent` (a requested window's token, or 0 for the session's window) or an application window in
     * the activity group `group`, with its buffer's size (0 x 0 for the frame's), format, alpha and dim. The session
     * answers with WINDOW_CREATED; a negative answer is an errno, -EOPNOTSUPP without a session.
     */
    public static int makeWindow(long token, int type, long parent, int left, int top, int right, int bottom,
            int width, int height, int format, int group, float alpha, float dim) {
        return requestWindow(WINDOW_MAKE, type, token, parent, left, top, right, bottom, width, height, format, group,
                alpha, dim);
    }

    /** @hide ViewRootImpl's: a new frame, size, alpha or dim; a new size is answered with WINDOW_RESIZED. */
    public static int placeWindow(long token, int type, long parent, int left, int top, int right, int bottom,
            int width, int height, int format, int group, float alpha, float dim) {
        return requestWindow(WINDOW_PLACE, type, token, parent, left, top, right, bottom, width, height, format,
                group, alpha, dim);
    }

    /** @hide ViewRootImpl's: the window goes, its sub-windows first; no answer. */
    public static int removeWindow(long token) {
        return requestWindow(WINDOW_REMOVE, 0, token, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0f, 0f);
    }

    private static int requestWindow(int op, int type, long token, long parent, int left, int top, int right,
            int bottom, int width, int height, int format, int group, float alpha, float dim) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestWindow(op, type, token, parent, left, top, right, bottom, width, height, format,
                group, Float.floatToRawIntBits(alpha), Float.floatToRawIntBits(dim));
    }

    /**
     * @hide NotificationService's toasts (ADR-0041 gap 4 (b4)): whether the app is in the foreground, as
     * ActivityTaskManager's hasResumedActivity answers for its toasts: the session resumed with an activity on top.
     */
    public boolean isAppForeground() {
        return mSessionResumed && topLiveRecord() != null;
    }

    /**
     * @hide WindowManagerGlobal's, at an application window's add (ADR-0041 gap 4 (b)): whether `token` is an
     * activity's that is not destroyed, as WMS finds its ActivityRecord; an unknown or destroyed one gives Android's
     * ADD_BAD_APP_TOKEN.
     */
    public boolean isActivityTokenValid(IBinder token) {
        final ActivityClientRecord r = token != null ? mActivities.get(token) : null;
        return r != null && r.activity != null && !r.activity.isDestroyed();
    }

    /**
     * @hide SystemPickerActivity's (ADR-0057 step a): the pick request `id` with its text for the host, which answers
     * with PICK_RESULT; 0, or a negative errno, -EOPNOTSUPP without a session.
     */
    public static int requestPick(long id, String text) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestPick(id, text.getBytes(StandardCharsets.UTF_8));
    }

    /**
     * @hide SystemPickerActivity's (ADR-0057 step a): the request `id` is not wanted any more, its activity being gone;
     * the host drops the picker. 0, or a negative errno, -EOPNOTSUPP without a session.
     */
    public static int cancelPick(long id) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestPickCancel(id);
    }

    /**
     * @hide SystemOpenActivity's (ADR-0062): the open request `id` with its text for the host, which answers with
     * OPEN_RESULT; 0, or a negative errno, -EOPNOTSUPP without a session.
     */
    public static int requestOpen(long id, String text) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestOpen(id, text.getBytes(StandardCharsets.UTF_8));
    }

    /**
     * @hide SystemOpenActivity's (ADR-0062): the request `id` is not wanted any more, its activity being gone; the host
     * drops what it shows. 0, or a negative errno, -EOPNOTSUPP without a session.
     */
    public static int cancelOpen(long id) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestOpenCancel(id);
    }

    /**
     * @hide SystemPermissionActivity's (ADR-0063): what the host answered so far for the groups it decides; null
     * before the thread.
     */
    public static PermissionAnswers permissionAnswers() {
        final ActivityThread thread = sCurrentActivityThread;
        return thread == null ? null : thread.mPackage.answers();
    }

    /**
     * @hide SystemPermissionActivity's: PermissionPolicy's grant for the app's permission; denied for good before the
     * thread.
     */
    public static PermissionPolicy.Grant permissionGrant(String permission) {
        final ActivityThread thread = sCurrentActivityThread;
        return thread == null ? PermissionPolicy.Grant.DENIED_FIXED : thread.mPackage.grantOf(permission);
    }

    /** @hide SystemPermissionActivity's: whether the app holds the permission now, as checkSelfPermission answers. */
    public static boolean permissionGranted(String permission) {
        final ActivityThread thread = sCurrentActivityThread;
        return thread != null && thread.mPackage.granted(permission);
    }

    /**
     * @hide SystemPermissionActivity's (ADR-0063): the permission request `id` with its text for the host, which
     * answers with PERMISSION_RESULT; 0, or a negative errno, -EOPNOTSUPP without a session.
     */
    public static int requestPermission(long id, String text) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestPermission(id, text.getBytes(StandardCharsets.UTF_8));
    }

    /**
     * @hide InputMethodManager's (ADR-0068 part 1): APKRT_REQUEST_IME, the served view's start, show, hide and stop for
     * the host's keyboard; 0, or a negative errno, -EOPNOTSUPP without a session.
     */
    public static int requestIme(long serial, String text) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestIme(serial, text.getBytes(StandardCharsets.UTF_8));
    }

    /**
     * @hide SystemPermissionActivity's: the request `id` is not wanted any more; the host drops its sheet, no
     * answer.
     */
    public static int cancelPermission(long id) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestPermissionCancel(id);
    }

    /**
     * @hide LocalLocationManager's (ADR-0063): begins or reconfigures continuous updates; 0, or a negative errno,
     * -EOPNOTSUPP without a session.
     */
    public static int locationStart(int accuracy, int distanceM) {
        return sessionOf() == null ? -EOPNOTSUPP : nativeLocationStart(accuracy, distanceM);
    }

    /**
     * @hide ManifestPackage's (ADR-0063): the runtime's stored answer for a permission group (0 denied or unanswered,
     * 1 granted, 2 granted coarse), which the host recorded before the app's first code ran; 0 when the runtime has
     * none.
     */
    public static int storedPermissionGrant(int group) {
        // No session needed: the answer is the runtime's, and the app's first code may run before the pipe is attached.
        return Math.max(0, nativeLocationGrant(group));
    }

    /** @hide LocalLocationManager's: ends continuous updates. */
    public static int locationStop() {
        return sessionOf() == null ? -EOPNOTSUPP : nativeLocationStop();
    }

    /** @hide LocalLocationManager's: one fix for `token`, which APKRT_EVENT_LOCATION answers. */
    public static int locationCurrent(long token) {
        return sessionOf() == null ? -EOPNOTSUPP : nativeLocationCurrent(token);
    }

    /** @hide LocalLocationManager's: the one-shot `token` is not wanted any more. */
    public static int locationCancel(long token) {
        return sessionOf() == null ? -EOPNOTSUPP : nativeLocationCancel(token);
    }

    /**
     * @hide LocalLocationManager's: the fix an event announced, into the arrays (nine doubles, four longs); false when
     * gone.
     */
    public static boolean locationGet(long sequence, double[] doubles, long[] longs) {
        return sessionOf() != null && nativeLocationFix(false, sequence, doubles, longs) == 1;
    }

    /** @hide LocalLocationManager's: the last continuous fix, into the arrays; false when there is none. */
    public static boolean locationLast(double[] doubles, long[] longs) {
        return sessionOf() != null && nativeLocationFix(true, 0, doubles, longs) == 1;
    }

    /**
     * @hide LocalBatteryService's: the host's battery as its level in percent (0xffffffff when unknown) | its state
     * (APKRT_BATTERY_STATE_*) << 32, or a negative errno, -ENODATA before the host's first value. No session needed:
     * the value is the runtime's.
     */
    public static long battery() {
        return nativeBattery();
    }

    /**
     * @hide LocalNetwork's: the host's network as APKRT_NETWORK_* flags, or a negative errno, -ENODATA before the
     * host's first state. No session needed: the state is the runtime's.
     */
    public static long network() {
        return nativeNetwork();
    }

    /**
     * @hide SystemBarInsets': the host's system bars as "<top>,<bottom>", which it passes in MINATO_BAR_INSETS and the
     * runtime takes out of the environment at start (apps do not see it), or null when it passed none.
     */
    public static String barInsets() {
        return nativeBarInsets();
    }

    /** The thread when a session drives it, else null: the requests to the host have no one to answer without. */
    private static ActivityThread sessionOf() {
        final ActivityThread thread = sCurrentActivityThread;
        return thread == null || thread.mSessionEvents == null ? null : thread;
    }

    /**
     * @hide MediaLibrary's (ADR-0057 step e): a finished image (kind 1) or video (kind 2) at `path` below the app's data
     * folder, for the host to add to Photos; no answer. 0, or a negative errno, -EOPNOTSUPP without a session.
     */
    public static int requestExportMedia(int kind, String path) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestExportMedia((kind + "\t" + path).getBytes(StandardCharsets.UTF_8));
    }

    /**
     * @hide NotificationService's (ADR-0074 decision 2): APKRT_REQUEST_NOTIFICATION, a post, cancel or cancel of all
     * with NotificationText's text, which the host shows on iOS; no answer. 0, or a negative errno, -EOPNOTSUPP without
     * a session, -EACCES without the app's grant.
     */
    public static int requestNotification(String text) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread != null && thread.mSessionEvents == null && !thread.mSessionResumed) {
            // Before the session: kept for attachSession, as the Application may post in its onCreate.
            synchronized (sEarlyNotifications) {
                if (sEarlyNotifications.size() < EARLY_NOTIFICATIONS_LIMIT) {
                    sEarlyNotifications.add(text.getBytes(StandardCharsets.UTF_8));
                }
            }
            return 0;
        }
        if (sessionOf() == null) {
            return -EOPNOTSUPP;
        }
        return nativeSessionRequestNotification(text.getBytes(StandardCharsets.UTF_8));
    }

    /**
     * setRequestedOrientation's request to the session (APKRT_REQUEST_ORIENTATION), as the window manager turns the
     * display for an activity's request: the host turns its window or ignores the value, and the turn arrives as the
     * session's configuration change.
     */
    void requestOrientation(int requestedOrientation) {
        if (mSessionEvents == null) {
            return;
        }
        int error = nativeSessionRequest(REQUEST_ORIENTATION, requestedOrientation, 0);
        if (error != 0) {
            Log.e(TAG, "The session refused ORIENTATION: " + error);
        }
    }

    public static void requestWindowFormat(int format) {
        final ActivityThread thread = sCurrentActivityThread;
        if (thread == null || thread.mSessionEvents == null) {
            return;
        }
        int error = nativeSessionRequest(REQUEST_WINDOW_FORMAT, format & 0xffffffffL, 0);
        if (error != 0) {
            Log.e(TAG, "The session refused WINDOW_FORMAT: " + error);
        }
    }

    /** As AOSP's performRestartActivity for a stopped activity: onRestart, then onStart. */
    private void performRestartActivity(ActivityClientRecord r) {
        r.activity.performRestart(true);
        r.state = ON_START;
    }

    /**
     * As AOSP's handlePauseActivity: the user leaves the activity, as at HOME or at a start without
     * FLAG_ACTIVITY_NO_USER_ACTION, but not at its finish; the top resumed state goes; then onPause. A target below 11
     * (pre-Honeycomb), whose state Android saves here, cannot be installed on Android 15.
     */
    private void handlePauseActivity(ActivityClientRecord r, boolean userLeaving) {
        final Activity activity = r.activity;
        if (userLeaving) {
            mInstrumentation.callActivityOnPictureInPictureRequested(activity);
            mInstrumentation.callActivityOnUserLeaving(activity);
        }
        reportTopResumedActivityChanged(r, false, "pausing");
        activity.mCalled = false;
        mInstrumentation.callActivityOnPause(activity);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + r.intent.getComponent().toShortString()
                    + " did not call through to super.onPause()");
        }
        r.state = ON_PAUSE;
    }

    /**
     * As AOSP's handleStopActivity: the activity's description, onStop with its state saved, the window hidden, and
     * the pending writes of SharedPreferences' apply() committed (QueuedWork).
     */
    private void handleStopActivity(ActivityClientRecord r) {
        r.activity.onCreateDescription();
        callActivityOnStop(r, true);
        updateVisibility(r, false);
        QueuedWork.waitToFinish();
    }

    /** As AOSP's callActivityOnStop: the state saved before onStop for a target below 28 (P), after it from 28 on. */
    private void callActivityOnStop(ActivityClientRecord r, boolean saveState) {
        final boolean shouldSaveState = saveState && !r.activity.mFinished && r.savedState == null;
        final boolean isPreP = r.activity.getApplicationInfo().targetSdkVersion < Build.VERSION_CODES.P;
        if (shouldSaveState && isPreP) {
            callActivityOnSaveInstanceState(r);
        }
        r.activity.performStop(false, "STOP_ACTIVITY_ITEM");
        r.state = ON_STOP;
        if (shouldSaveState && !isPreP) {
            callActivityOnSaveInstanceState(r);
        }
    }

    private void callActivityOnSaveInstanceState(ActivityClientRecord r) {
        r.savedState = new Bundle();
        mInstrumentation.callActivityOnSaveInstanceState(r.activity, r.savedState);
    }

    /**
     * As AOSP's updateVisibility: for a hidden window the decor goes invisible until the next resume shows it; for a
     * window shown again without a resume, as below a translucent activity (gap 4 (b3)), it is visible at once.
     */
    private void updateVisibility(ActivityClientRecord r, boolean show) {
        View decor = r.activity.mDecor;
        if (decor != null && !show && r.activity.mVisibleFromServer) {
            r.activity.mVisibleFromServer = false;
            decor.setVisibility(View.INVISIBLE);
        } else if (decor != null && show && !r.activity.mVisibleFromServer) {
            r.activity.mVisibleFromServer = true;
            if (r.activity.mVisibleFromClient) {
                decor.setVisibility(View.VISIBLE);
            }
        }
    }

    /**
     * As AOSP's handleDestroyActivity for a finishing activity: paused and stopped first if it was not, onDestroy,
     * then its window removed.
     */
    private void handleDestroyActivity(ActivityClientRecord r) {
        handleDestroyActivity(r, false);
    }

    /**
     * The same for a relaunch (`relaunching`): the activity is not finishing, its record stays in the task and what
     * getLastNonConfigurationInstance gives goes to the next instance; isChangingConfigurations is true.
     */
    private void handleDestroyActivity(ActivityClientRecord r, boolean relaunching) {
        final Activity activity = r.activity;
        if (relaunching) {
            activity.mChangingConfigurations = true;
        } else {
            activity.mFinished = true;
        }
        if (r.state == ON_RESUME) {
            handlePauseActivity(r, false);
        }
        if (r.state == ON_PAUSE) {
            callActivityOnStop(r, false);
        }
        if (relaunching) {
            r.lastNonConfigurationInstances = activity.retainNonConfigurationInstances();
        }
        activity.mCalled = false;
        mInstrumentation.callActivityOnDestroy(activity);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + r.intent.getComponent().toShortString()
                    + " did not call through to super.onDestroy()");
        }
        if (r.window != null) {
            r.window.closeAllPanels();
        }
        r.state = ON_DESTROY;
        if (!relaunching) {
            removeRecord(r);
        }
        View decor = activity.mDecor;
        if (decor != null) {
            if (activity.mWindowAdded) {
                ViewRootImpl root = decor.getViewRootImpl();
                if (root != null) {
                    root.setActivityConfigCallback(null);
                }
                if (mWindowRoot != null && decor.getParent() == mWindowRoot) {
                    mWindowRoot = null;
                }
                if (mHostRoot != null && decor.getParent() == mHostRoot) {
                    // The window that holds the session's: the next top gets them without it, and a share of the
                    // input channel stays open for it, as the host connects the queue once.
                    WindowManagerGlobal.getInstance().forgetSessionWindow(mHostRoot);
                    mHostRoot = null;
                    mHostInput = mHostInput != null ? mHostInput.dup() : null;
                }
                activity.getWindowManager().removeViewImmediate(decor);
            }
            activity.mDecor = null;
        }
        // As AOSP's: the receivers it registered and left go after this message (ADR-0045 decision 5, step d2).
        Context c = activity.getBaseContext();
        if (c instanceof ContextImpl) {
            ((ContextImpl) c).scheduleFinalCleanup(activity.getClass().getName(), "Activity");
        }
    }

    /**
     * As AOSP's handleTrimMemory (ActivityThread.collectComponentCallbacks's order): onTrimMemory(level) of the
     * Application, which passes it to the callbacks registered with it, of the activities that are not finishing, of
     * the services and of the providers.
     */
    private void handleTrimMemory(int level) {
        mInitialApplication.onTrimMemory(level);
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            Activity activity = mActivities.valueAt(i).activity;
            if (activity != null && !activity.mFinished) {
                activity.onTrimMemory(level);
            }
        }
        for (int i = 0; i < mServices.size(); i++) {
            mServices.valueAt(i).onTrimMemory(level);
        }
        java.util.Set<ContentProvider> providers = new java.util.LinkedHashSet<>(mLocalProvidersByAuthority.values());
        for (ContentProvider provider : providers) {
            provider.onTrimMemory(level);
        }
    }

    /**
     * As AOSP's handleLowMemory: onLowMemory of the Application, of the activities that are not finishing and of the
     * providers. Android then frees SQLite's, the text layout's and the canvas's caches, which Minato leaves alone.
     */
    private void handleLowMemory() {
        mInitialApplication.onLowMemory();
        for (int i = mActivities.size() - 1; i >= 0; i--) {
            Activity activity = mActivities.valueAt(i).activity;
            if (activity != null && !activity.mFinished) {
                activity.onLowMemory();
            }
        }
        java.util.Set<ContentProvider> providers = new java.util.LinkedHashSet<>(mLocalProvidersByAuthority.values());
        for (ContentProvider provider : providers) {
            provider.onLowMemory();
        }
    }

    /** As AOSP's reportTopResumedActivityChanged: onTopResumedActivityChanged when the reported state changes. */
    private void reportTopResumedActivityChanged(ActivityClientRecord r, boolean onTop, String reason) {
        if (r.lastReportedTopResumedState != onTop) {
            r.lastReportedTopResumedState = onTop;
            r.activity.performTopResumedActivityChanged(onTop, reason);
        }
    }

    /** The session's pipe, as session_connect gives it; null without a session (-ENOENT). Throws for other errors. */
    private static native FileDescriptor nativeSessionConnect();

    /** Reads one event into type, serial and its three arguments; false when the pipe broke. */
    private static native boolean nativeSessionRead(FileDescriptor events, long[] event);

    /** Answers the event of `serial`, whose callbacks returned. */
    private static native void nativeSessionAnswer(int serial);

    /** session_request: 0, or the negative errno. */
    private static native int nativeSessionRequest(int type, long a0, long a1);

    /** APKRT_REQUEST_PICK with its text, as UTF-8, and ID (ADR-0057 step a). */
    private static native int nativeSessionRequestPick(long id, byte[] text);

    /** APKRT_REQUEST_PICK_CANCEL (ADR-0057 step a). */
    private static native int nativeSessionRequestPickCancel(long id);

    /** APKRT_REQUEST_OPEN with its text, as UTF-8, and ID (ADR-0062). */
    private static native int nativeSessionRequestOpen(long id, byte[] text);

    /** APKRT_REQUEST_OPEN_CANCEL (ADR-0062). */
    private static native int nativeSessionRequestOpenCancel(long id);

    private static native int nativeSessionRequestPermission(long id, byte[] text);

    private static native int nativeSessionRequestPermissionCancel(long id);

    private static native int nativeSessionRequestIme(long serial, byte[] text);

    private static native int nativeLocationStart(int accuracy, int distanceM);

    private static native int nativeLocationGrant(int group);

    private static native int nativeLocationStop();

    private static native int nativeLocationCurrent(long token);

    private static native int nativeLocationCancel(long token);

    /** GET (the fix of `sequence`) or LAST into nine doubles and four longs; 1 filled, 0 none, or a negative errno. */
    private static native int nativeLocationFix(boolean last, long sequence, double[] doubles, long[] longs);

    /** APKRT_NR_BATTERY's GET: level | state << 32, or a negative errno. */
    private static native long nativeBattery();
    private static native long nativeNetwork();
    private static native String nativeBarInsets();
    private static native int nativeSessionRequestExportMedia(byte[] text);

    private static native int nativeSessionRequestNotification(byte[] text);

    /** APKRT_REQUEST_SURFACE with its struct apkrt_surface_request, which the native fills (ADR-0043 c4). */
    private static native int nativeSessionRequestSurface(int op, int z, long token, int left, int top, int right,
            int bottom, int width, int height, int format);

    /** APKRT_REQUEST_WINDOW with its struct apkrt_window_request (ADR-0041 gap 4 (b)); alpha and dim as bits. */
    private static native int nativeSessionRequestWindow(int op, int type, long token, long parent, int left, int top,
            int right, int bottom, int width, int height, int format, int group, int alpha, int dim);

    /**
     * input_connect of the session's input channel `id` (ADR-0045 step 7b2), as an InputChannel named `name`; null
     * when the runtime refuses the ID.
     */
    private static native InputChannel nativeSessionConnectInput(long id, String name);

    /** Ends the process at once, as activity-host's _exit does: Android kills the app's process later anyway. */
    private static native void nativeExit(int status);

    /** The process's binder, which Activity hands to Instrumentation.execStartActivity; no system_server calls it. */
    final class ApplicationThread extends IApplicationThread.Stub {
    }

    private final ApplicationThread mAppThread = new ApplicationThread();

    public ApplicationThread getApplicationThread() {
        return mAppThread;
    }

    public Instrumentation getInstrumentation() {
        return mInstrumentation;
    }

    public Looper getLooper() {
        return Looper.getMainLooper();
    }

    public Handler getHandler() {
        return sMainThreadHandler;
    }

    public String getProcessName() {
        return mProcessName;
    }

    /** As AOSP's for an app without a profiler: Minato starts none. */
    public boolean isProfiling() {
        return false;
    }

    public String getProfileFilePath() {
        return null;
    }

    /** The bind's data that Instrumentation reads: never an SDK sandbox's on Minato. */
    static final class AppBindData {
        boolean isSdkInSandbox;
    }

    final AppBindData mBoundApplication = new AppBindData();

    /** The app's own package, the only one a Minato process loads; null for any other, as AOSP's for one not loaded. */
    public final LoadedApk peekPackageInfo(String packageName, boolean includeCode) {
        return mPackageName.equals(packageName) ? mLoadedApk : null;
    }

    /**
     * A result for an activity, as an Instrumentation monitor's answer to a start gives it: delivered as any result
     * (step b3.5b2-2a), on the main thread.
     */
    public final void sendActivityResult(IBinder token, String id, int requestCode, int resultCode, Intent data) {
        final Intent copy = data != null ? new Intent(data) : null;
        sMainThreadHandler.post(() -> {
            final ActivityClientRecord r = mActivities.get(token);
            if (r != null) {
                sendResult(r, id, requestCode, resultCode, copy);
            }
        });
    }

    /** Activity.recreate's relaunch: on the main thread, after what the app posted before it. */
    public void scheduleRelaunchActivity(IBinder token) {
        sMainThreadHandler.post(() -> {
            final ActivityClientRecord r = mActivities.get(token);
            if (r != null && r.activity != null && !r.activity.mFinished) {
                relaunchActivity(r);
            }
        });
    }

    /**
     * The screen turned (the session's size, from the window the host resized): as the window manager's
     * configuration change and AOSP's handleConfigurationChanged have it. Display 0 has the new size, the system
     * Resources and the app's, the Application and the other components hear of it, and each activity of the task,
     * the top first, is relaunched when its manifest's configChanges do not cover the change and hears
     * onConfigurationChanged and gets its window laid out again when they do.
     */
    @SuppressWarnings("deprecation") // Resources.updateConfiguration, whose public form updates a Resources' impl.
    private void handleTurn(int width, int height) {
        final Resources system = Resources.getSystem();
        final DisplayMetrics now = system.getDisplayMetrics();
        if (width <= 0 || height <= 0 || (now.widthPixels == width && now.heightPixels == height)) {
            return;
        }
        final Configuration before = new Configuration(system.getConfiguration());
        final Configuration config = new Configuration(before);
        config.seq = 0;
        config.orientation = width > height ? Configuration.ORIENTATION_LANDSCAPE
                : Configuration.ORIENTATION_PORTRAIT;
        final float density = config.densityDpi * DisplayMetrics.DENSITY_DEFAULT_SCALE;
        config.screenWidthDp = (int) (width / density);
        config.screenHeightDp = (int) (height / density);
        final DisplayMetrics metrics = new DisplayMetrics();
        metrics.setTo(now);
        metrics.widthPixels = metrics.noncompatWidthPixels = width;
        metrics.heightPixels = metrics.noncompatHeightPixels = height;
        final int changes = before.diffPublicOnly(config);
        DisplayService.turn(width, height);
        DisplayManagerGlobal.getInstance().handleDisplayChangeFromWindowManager(Display.DEFAULT_DISPLAY);
        Resources.updateSystemConfiguration(config, metrics, null);
        if (mLoadedApk != null) {
            mLoadedApk.getResources().updateConfiguration(config, metrics);
        }
        mInitialApplication.onConfigurationChanged(config);
        for (ActivityClientRecord r : reversedTask()) {
            if (r.activity == null || r.activity.mFinished || r.state < ON_CREATE || r.state > ON_STOP) {
                continue;
            }
            r.overrideConfig = windowConfiguration();
            if ((changes & ~r.activityInfo.getRealConfigChanged()) != 0) {
                relaunchActivity(r);
            } else {
                performConfigurationChanged(r, config, metrics);
            }
        }
        for (int i = 0; i < mServices.size(); i++) {
            mServices.valueAt(i).onConfigurationChanged(config);
        }
        java.util.Set<ContentProvider> providers = new java.util.LinkedHashSet<>(mLocalProvidersByAuthority.values());
        for (ContentProvider provider : providers) {
            provider.onConfigurationChanged(config);
        }
    }

    /**
     * As AOSP's performActivityConfigurationChanged for an activity that handles the change itself: its Resources
     * have the new configuration and metrics before onConfigurationChanged runs, and its window is laid out again with
     * the focus gone and back, as the window manager's relayout does.
     */
    @SuppressWarnings("deprecation") // Resources.updateConfiguration, as handleTurn.
    private void performConfigurationChanged(ActivityClientRecord r, Configuration config, DisplayMetrics metrics) {
        final Activity activity = r.activity;
        final Configuration merged = new Configuration(config);
        merged.updateFrom(r.overrideConfig);
        activity.getResources().updateConfiguration(merged, metrics);
        activity.mConfigChangeFlags = 0;
        activity.mCurrentConfig = new Configuration(merged);
        activity.mCalled = false;
        activity.onConfigurationChanged(merged);
        if (!activity.mCalled) {
            throw new SuperNotCalledException("Activity " + activity.getLocalClassName()
                    + " did not call through to super.onConfigurationChanged()");
        }
        if (r.state == ON_RESUME && mHostRoot != null && rootOf(r) == mHostRoot) {
            // The window's size came with the resize before: its relayout is scheduled, the focus goes and returns
            // in a message of its own, as the window that is laid out again hears of both.
            final ViewRootImpl turned = mHostRoot;
            WindowManagerGlobal.getInstance().setSessionWindow(turned, false);
            sMainThreadHandler.post(() -> {
                if (mHostRoot == turned) {
                    WindowManagerGlobal.getInstance().setSessionWindow(turned, mHostFocus);
                }
            });
        }
    }

    /**
     * As AOSP's handleRelaunchActivity: a resumed activity pauses, stops with its state saved and is destroyed with
     * isChangingConfigurations true; a new instance of the same record is created with that state and non-configuration
     * instance, started with onRestoreInstanceState, and resumed again. A paused or stopped one comes back stopped.
     */
    private void relaunchActivity(ActivityClientRecord r) {
        final int before = r.state;
        if (before != ON_RESUME && before != ON_PAUSE && before != ON_STOP) {
            return;
        }
        if (before == ON_RESUME) {
            handlePauseActivity(r, false);
        }
        if (r.state == ON_PAUSE) {
            callActivityOnStop(r, true);
        }
        final Bundle saved = r.savedState;
        handleDestroyActivity(r, true);
        r.activity = null;
        r.window = null;
        r.windowsApplied = false;
        r.launchStartTime = 0;
        r.overrideConfig = windowConfiguration();
        r.restoredState = saved;
        if (performLaunchActivity(r) == null) {
            removeRecord(r);
            return;
        }
        handleStartActivity(r);
        if (before == ON_RESUME) {
            handleResumeActivity(r);
            updateWindows();
        } else {
            callActivityOnStop(r, false);
            updateWindows();
        }
    }

    /** Instrumentation.finish: no instrumentation test runs on Minato. */
    final void finishInstrumentation(int resultCode, android.os.Bundle results) {
        Log.w(TAG, "No instrumentation to finish: " + resultCode);
    }

}
