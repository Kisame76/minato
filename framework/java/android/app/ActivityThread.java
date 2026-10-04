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
 * handleBindApplication does. The launcher activity comes in step c2c3, the lifecycle host in step c3.
 *
 *   ActivityThread.main --package <package> --process <name> --apk <path of base.apk>
 *                       --library-path <lib/arm64 beside it> --data-dir <the app's data folder>
 *                       [--signers <the version's signers.json>] [--application <class>]
 */
package android.app;

import android.content.ContentProvider;
import android.content.Context;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.IBinder;
import android.os.LocaleList;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.util.ArrayMap;
import android.util.DisplayMetrics;
import android.util.Log;
import dalvik.system.RuntimeHooks;
import dalvik.system.PathClassLoader;
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
    private Application mInitialApplication;

    /** The app's providers by authority, as installProvider registers them for acquireProvider (step c2d). */
    final ArrayMap<String, ContentProvider> mLocalProvidersByAuthority = new ArrayMap<>();

    /**
     * The activities by their token, as AOSP's ActivityThread keeps them, which Application.getAutofillClient walks:
     * none until step c2c launches the first.
     */
    final ArrayMap<IBinder, ActivityClientRecord> mActivities = new ArrayMap<>();

    /** An activity's record, with what AOSP's Application reads; step c2c's launch makes and fills it. */
    public static final class ActivityClientRecord {
        Activity activity;
        boolean isTopResumedActivity;
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

    /** The broadcast the main thread is receiving, which StrictMode names in a violation: none, Minato has none yet. */
    public static Intent getIntentBeingBroadcast() {
        return null;
    }

    /** Whether this is system_server's thread: never in an app process. WindowConfiguration asks it. */
    public static boolean isSystem() {
        return false;
    }

    @SuppressWarnings("deprecation") // Looper.prepareMainLooper, deprecated for apps, which only this main calls.
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
        for (int i = 0; i + 1 < args.length; i += 2) {
            switch (args[i]) {
                case "--package": packageName = args[i + 1]; break;
                case "--process": processName = args[i + 1]; break;
                case "--apk": apk = args[i + 1]; break;
                case "--library-path": libraryPath = args[i + 1]; break;
                case "--application": application = args[i + 1]; break;
                case "--data-dir": dataDir = args[i + 1]; break;
                case "--signers": signers = args[i + 1]; break;
                default: throw new IllegalArgumentException("ActivityThread.main: unknown argument " + args[i]);
            }
        }
        if (packageName == null || apk == null || libraryPath == null || dataDir == null || args.length % 2 != 0) {
            throw new IllegalArgumentException(
                    "usage: ActivityThread.main --package <package> [--process <name>] --apk <base.apk> "
                    + "--library-path <folder> --data-dir <folder> [--signers <signers.json>] "
                    + "[--application <class>]");
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
        // As handleBindApplication: the target SDK for hwui's rendering, then the system fonts. Android loads the
        // preinstalled font map only when system_server's serialized one fails; with no system_server, ours always
        // loads it, from /system/etc's lists and /system/fonts. AOSP does both before it applies the resources'
        // configuration, and ours after, since ManifestPackage.read, which gives the target SDK, reads the manifest
        // through Resources; neither reads that configuration.
        android.graphics.Compatibility.setTargetSdkVersion(pkg.applicationInfo(0).targetSdkVersion);
        Typeface.loadPreinstalledSystemFontMap();
        ClassLoader loader = new PathClassLoader(apk, libraryPath, ClassLoader.getSystemClassLoader());
        ActivityThread thread = new ActivityThread(packageName, processName != null ? processName : packageName,
                loader, pkg);
        sCurrentActivityThread = thread;
        // As handleBindApplication's makeApplicationInner (step c1): the manifest's Application class, else
        // android.app.Application, through the app's AppComponentFactory, which loads and initializes it, then its
        // attachBaseContext with the app's ContextImpl. currentApplication() gives it out from then on.
        LoadedApk packageInfo = new LoadedApk(thread, pkg.applicationInfo(0), loader);
        Application app;
        try {
            app = packageInfo.makeApplication(application);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException("Unable to instantiate application " + application + " package " + packageName
                    + ": " + e, e);
        }
        thread.mInitialApplication = app;
        // As handleBindApplication: the process's providers, before the Application's onCreate (step c2c1).
        thread.installContentProviders(app, packageInfo, pkg.processProviders(thread.mProcessName));
        // As handleBindApplication after the providers: Instrumentation.callApplicationOnCreate, which calls
        // onCreate and nothing else, and Android's wrapping of what it throws. AOSP's Instrumentation comes with
        // Activity in step c2c3, whose closure it needs, and then makes this call.
        try {
            app.onCreate();
        } catch (Exception e) {
            throw new RuntimeException("Unable to create application " + app.getClass().getName() + ": " + e, e);
        }
        // As AOSP's main: the Looper runs the app's messages until the process ends. Android ends it from outside (am
        // force-stop, a kill); so do Minato's host and the checks on the Mac (framework/tests/app_process_run.py).
        Looper.loop();
        throw new RuntimeException("Main thread loop unexpectedly exited");
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
}
