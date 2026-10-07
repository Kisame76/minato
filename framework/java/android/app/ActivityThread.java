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
 * its ViewRootImpl. The root activity's finish() asks the host, which answers with the quit. Without a session the
 * launch runs by itself as before. Since ADR-0045's gap 13a it gives ContentResolver the app's providers and the
 * local settings provider (acquireProvider), whose android_id is the host's --android-id. Since step c2d3 it gives
 * the main thread and the process StrictMode's defaults for the app's target SDK, and allows disk writes while the
 * Application is made and created, as handleBindApplication does.
 *
 *   ActivityThread.main --package <package> --process <name> --apk <path of base.apk>
 *                       --library-path <lib/arm64 beside it> --data-dir <the app's data folder>
 *                       [--signers <the version's signers.json>] [--application <class>]
 *                       [--android-id <16 hex digits>]
 */
package android.app;

import android.app.compat.CompatChanges;
import android.content.AttributionSource;
import android.content.ContentProvider;
import android.content.Context;
import android.content.ComponentName;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.hardware.display.DisplayManagerGlobal;
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
import android.permission.IPermissionManager;
import android.provider.DeviceConfig;
import android.provider.Settings;
import android.provider.SettingsProvider;
import android.system.ErrnoException;
import android.system.Os;
import android.text.TextFlags;
import android.util.ArrayMap;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.Slog;
import android.util.SuperNotCalledException;
import android.util.UtilConfig;
import android.view.Display;
import android.view.InputChannel;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewManager;
import android.view.ViewRootImpl;
import android.view.Window;
import android.view.WindowManager;
import android.widget.WidgetFlags;
import com.android.internal.compat.CompatTable;
import com.android.internal.os.SafeZipPathValidatorCallback;
import dalvik.system.AppSpecializationHooks;
import dalvik.system.RuntimeHooks;
import dalvik.system.PathClassLoader;
import dalvik.system.VMRuntime;
import dalvik.system.ZipPathValidator;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.List;
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
    /** The task's root activity and its top resumed one, which LocalActivityClientController answers for. */
    IBinder mTaskRootToken;
    IBinder mTopToken;

    /** The app's providers by authority, as installProvider registers them for acquireProvider (step c2d). */
    final ArrayMap<String, ContentProvider> mLocalProvidersByAuthority = new ArrayMap<>();

    /** The host's android_id (--android-id), or null; the settings provider then makes one (ADR-0045, gap 13a). */
    private String mAndroidId;
    /** The local settings provider, made at its first acquire; under mLocalProvidersByAuthority's lock. */
    private SettingsProvider mSettingsProvider;

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

    // Looper.prepareMainLooper and AsyncTask's executors, deprecated for apps, which only this main calls.
    @SuppressWarnings("deprecation")
    public static void main(String[] args) throws ErrnoException {
        // As RuntimeInit.commonInit sets them up for every app before ActivityThread.main (step c1).
        LoggingHandler logging = new LoggingHandler();
        RuntimeHooks.setUncaughtExceptionPreHandler(logging);
        Thread.setDefaultUncaughtExceptionHandler(new KillApplicationHandler(logging));
        String packageName = null;
        String processName = null;
        String apk = null;
        String libraryPath = null;
        String application = null;
        String dataDir = null;
        String signers = null;
        String androidId = null;
        for (int i = 0; i + 1 < args.length; i += 2) {
            switch (args[i]) {
                case "--package": packageName = args[i + 1]; break;
                case "--process": processName = args[i + 1]; break;
                case "--apk": apk = args[i + 1]; break;
                case "--library-path": libraryPath = args[i + 1]; break;
                case "--application": application = args[i + 1]; break;
                case "--data-dir": dataDir = args[i + 1]; break;
                case "--signers": signers = args[i + 1]; break;
                case "--android-id": androidId = args[i + 1]; break;
                default: throw new IllegalArgumentException("ActivityThread.main: unknown argument " + args[i]);
            }
        }
        if (packageName == null || apk == null || libraryPath == null || dataDir == null || args.length % 2 != 0
                || androidId != null && !androidId.matches("[0-9a-f]{16}")) {
            throw new IllegalArgumentException(
                    "usage: ActivityThread.main --package <package> [--process <name>] --apk <base.apk> "
                    + "--library-path <folder> --data-dir <folder> [--signers <signers.json>] "
                    + "[--application <class>] [--android-id <16 hex digits>]");
        }
        // As AOSP's main: the main thread's Looper, and the Handler that framework classes post to it with.
        Looper.prepareMainLooper();
        sMainThreadHandler = new Handler(Looper.myLooper());
        // Context.getCacheDir's folder, which installd creates at the install.
        String tmpdir = dataDir + "/cache";
        System.setProperty("java.io.tmpdir", tmpdir);
        Os.setenv("TMPDIR", tmpdir, true);
        applySessionConfiguration();
        // The import's signers.json beside the version (ADR-0034 decision 2, b3.5b1); an unsigned app has none.
        ManifestPackage pkg;
        try {
            pkg = ManifestPackage.read(apk, libraryPath, dataDir, signers);
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
        ClassLoader loader = new PathClassLoader(apk, libraryPath, ClassLoader.getSystemClassLoader());
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
            thread.mLoadedApk = packageInfo;
            Application app = packageInfo.makeApplication(application);
            thread.mInitialApplication = app;
            // As handleBindApplication: AOSP's Instrumentation for the process, and the activity client controller,
            // which Android's first launch transaction hands over; Minato's is the process's own (step c2c3).
            thread.mInstrumentation = new Instrumentation();
            thread.mInstrumentation.basicInit(thread);
            ActivityClient.setActivityClientController(new LocalActivityClientController(thread));
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
                provider = packageInfo.getAppFactory().instantiateProvider(context.getClassLoader(), info.name);
                provider.attachInfo(context, info);
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
        ActivityClientRecord r = new ActivityClientRecord();
        r.token = new Binder();
        r.intent = intent;
        r.activityInfo = info;
        r.packageInfo = mLoadedApk;
        r.overrideConfig = windowConfiguration();
        r.requestedOrientation = info.screenOrientation;
        mActivities.put(r.token, r);
        if (mTaskRootToken == null) {
            mTaskRootToken = r.token;
        }
        return performLaunchActivity(r) != null ? r : null;
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
                        title, null, null, null, config, null, null, null, null, null, null, null);
                activity.mStartedActivity = false;
                int theme = aInfo.getThemeResource();
                if (theme != 0) {
                    activity.setTheme(theme);
                }
                activity.mCalled = false;
                // As AOSP's: the record knows the activity before onCreate, for the lifecycle callbacks.
                r.activity = activity;
                mInstrumentation.callActivityOnCreate(activity, null);
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

    /** As AOSP's handleStartActivity for a new activity: onStart, then onPostCreate, as there is no saved state. */
    private void handleStartActivity(ActivityClientRecord r) {
        final Activity activity = r.activity;
        activity.performStart("handleStartActivity");
        r.state = ON_START;
        activity.mCalled = false;
        mInstrumentation.callActivityOnPostCreate(activity, null);
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
        a.performResume(false, "RESUME_ACTIVITY");
        r.state = ON_RESUME;
        r.savedState = null;
        reportTopResumedActivityChanged(r, r.isTopResumedActivity, "topWhenResuming");
        mTopToken = r.token;
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
    private static final int EVENT_NATIVE_WINDOW_REDRAW_NEEDED = 11;
    private static final int EVENT_NATIVE_WINDOW_DESTROYED = 12;
    private static final int EVENT_INPUT_QUEUE_CREATED = 13;
    private static final int EVENT_INPUT_QUEUE_DESTROYED = 14;
    private static final int EVENT_LOW_MEMORY = 17;
    private static final int EVENT_SURFACE_CREATED = 18;
    private static final int EVENT_SURFACE_RESIZED = 19;
    private static final int EVENT_SURFACE_DESTROYED = 20;
    private static final int REQUEST_FINISH = 1;
    private static final int REQUEST_WINDOW_FORMAT = 6;
    private static final int SURFACE_MAKE = 1;
    private static final int SURFACE_PLACE = 2;
    private static final int SURFACE_REMOVE = 3;
    /** Linux's EOPNOTSUPP: without a session, a host gives no surfaces. */
    private static final int EOPNOTSUPP = 95;

    /** The session's event pipe; null without a session. */
    private FileDescriptor mSessionEvents;
    /** The launcher activity's record, which the session's events drive. */
    private ActivityClientRecord mSessionActivity;
    /** The event types answered without more, each logged once: the content's rectangle and turns. */
    private final boolean[] mSkippedEvents = new boolean[32];

    /**
     * Connects to the host's session, as activity-host does at its start, and reads its events from then on. Without a
     * session (a host that began none, such as the Mac's checks without a configuration), the launcher activity starts
     * by itself, as before.
     */
    private void attachSession() {
        FileDescriptor events = nativeSessionConnect();
        if (events == null) {
            sMainThreadHandler.post(this::handleLaunchLauncherActivity);
            return;
        }
        mSessionEvents = events;
        ViewRootImpl.setHostWindows(true);
        watchSession();
    }

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
        final ActivityClientRecord r = mSessionActivity;
        final ViewRootImpl root = r != null && r.window != null ? r.window.getDecorView().getViewRootImpl() : null;
        switch (type) {
            case EVENT_CREATE:
                if (r == null) {
                    mSessionActivity = launchLauncherActivity();
                }
                break;
            case EVENT_START:
                if (r != null && r.state == ON_CREATE) {
                    handleStartActivity(r);
                } else if (r != null && r.state == ON_STOP) {
                    performRestartActivity(r);
                }
                break;
            case EVENT_RESUME:
                if (r != null && r.state == ON_START) {
                    handleResumeActivity(r);
                    handleTopResumedActivityChanged(r, true);
                }
                break;
            case EVENT_PAUSE:
                if (r != null && r.state == ON_RESUME) {
                    handlePauseActivity(r);
                }
                break;
            case EVENT_STOP:
                if (r != null && r.state == ON_PAUSE) {
                    handleStopActivity(r);
                }
                break;
            case EVENT_DESTROY:
                if (r != null) {
                    handleDestroyActivity(r);
                }
                break;
            case EVENT_SAVE_INSTANCE_STATE:
                // As Android's: the stop saved the state, before onStop for a target below 28 and after it from 28 on.
                break;
            case EVENT_WINDOW_FOCUS_CHANGED:
                if (root != null) {
                    root.windowFocusChanged(event[2] != 0);
                }
                break;
            case EVENT_NATIVE_WINDOW_CREATED:
            case EVENT_NATIVE_WINDOW_RESIZED:
                if (root != null) {
                    root.setHostWindow(event[2], (int) event[3], (int) event[4]);
                }
                break;
            case EVENT_NATIVE_WINDOW_REDRAW_NEEDED:
                if (root != null) {
                    root.redrawNeeded();
                }
                break;
            case EVENT_NATIVE_WINDOW_DESTROYED:
                if (root != null) {
                    root.setHostWindow(0, 0, 0);
                }
                break;
            case EVENT_INPUT_QUEUE_CREATED:
            case EVENT_INPUT_QUEUE_DESTROYED:
                // The window's touches (ADR-0045 step 7b2): the session's input channel as an InputChannel, none
                // for channel 0 and at the queue's end.
                if (root != null) {
                    InputChannel channel = null;
                    if (type == EVENT_INPUT_QUEUE_CREATED && event[2] != 0) {
                        channel = nativeSessionConnectInput(event[2],
                                String.valueOf(root.mWindowAttributes.getTitle()));
                        if (channel == null) {
                            Log.w(TAG, "The session's input channel " + event[2] + " did not connect: no touches");
                        }
                    }
                    root.setInputChannel(channel);
                } else if (type == EVENT_INPUT_QUEUE_CREATED && event[2] != 0) {
                    Log.w(TAG, "The session's input channel came before a window: no touches");
                }
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

    /** The root activity's finish(): with a session, the host answers it with the quit; false without one. */
    boolean requestFinish(IBinder token) {
        if (mSessionEvents == null || token != mTaskRootToken) {
            return false;
        }
        int error = nativeSessionRequest(REQUEST_FINISH, 0, 0);
        if (error != 0) {
            Log.e(TAG, "The session refused FINISH: " + error);
            return false;
        }
        return true;
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
     * As AOSP's handlePauseActivity: the user leaves an activity that is not finishing, as at HOME; the top resumed
     * state goes; then onPause. A target below 11 (pre-Honeycomb), whose state Android saves here, cannot be installed
     * on Android 15.
     */
    private void handlePauseActivity(ActivityClientRecord r) {
        final Activity activity = r.activity;
        if (!activity.mFinished) {
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

    /** As AOSP's updateVisibility for a hidden window: the decor goes invisible until the next resume shows it. */
    private void updateVisibility(ActivityClientRecord r, boolean show) {
        View decor = r.activity.mDecor;
        if (decor != null && !show && r.activity.mVisibleFromServer) {
            r.activity.mVisibleFromServer = false;
            decor.setVisibility(View.INVISIBLE);
        }
    }

    /**
     * As AOSP's handleDestroyActivity for a finishing activity: paused and stopped first if it was not, onDestroy,
     * then its window removed.
     */
    private void handleDestroyActivity(ActivityClientRecord r) {
        final Activity activity = r.activity;
        activity.mFinished = true;
        if (r.state == ON_RESUME) {
            handlePauseActivity(r);
        }
        if (r.state == ON_PAUSE) {
            callActivityOnStop(r, false);
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
        mActivities.remove(r.token);
        View decor = activity.mDecor;
        if (decor != null) {
            if (activity.mWindowAdded) {
                ViewRootImpl root = decor.getViewRootImpl();
                if (root != null) {
                    root.setActivityConfigCallback(null);
                }
                activity.getWindowManager().removeViewImmediate(decor);
            }
            activity.mDecor = null;
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

    /** APKRT_REQUEST_SURFACE with its struct apkrt_surface_request, which the native fills (ADR-0043 c4). */
    private static native int nativeSessionRequestSurface(int op, int z, long token, int left, int top, int right,
            int bottom, int width, int height, int format);

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

    /** A result for an activity started for one, which comes with step b3.5b2's intents. */
    public final void sendActivityResult(IBinder token, String id, int requestCode, int resultCode, Intent data) {
        Log.w(TAG, "No activity results yet (ADR-0039 b3.5b2): " + requestCode + " for " + token);
    }

    /** Activity.recreate's relaunch, which comes with step c3's lifecycle host. */
    public void scheduleRelaunchActivity(IBinder token) {
        Log.w(TAG, "No relaunch yet (ADR-0039 step c3): " + token);
    }

    /** Instrumentation.finish: no instrumentation test runs on Minato. */
    final void finishInstrumentation(int resultCode, android.os.Bundle results) {
        Log.w(TAG, "No instrumentation to finish: " + resultCode);
    }

}
