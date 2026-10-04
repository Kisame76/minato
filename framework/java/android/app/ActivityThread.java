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
 * handleBindApplication does. It creates no Context and runs no Looper or lifecycle; those come in step c.
 *
 *   ActivityThread.main --package <package> --process <name> --apk <path of base.apk>
 *                       --library-path <lib/arm64 beside it> --data-dir <the app's data folder>
 *                       [--signers <the version's signers.json>] [--application <class>]
 */
package android.app;

import android.content.pm.IPackageManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Typeface;
import android.os.LocaleList;
import android.system.ErrnoException;
import android.system.Os;
import android.util.DisplayMetrics;
import dalvik.system.PathClassLoader;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParserException;

public final class ActivityThread {
    /** As AOSP's: off. ResourcesManager logs configuration changes when it is on (b3.4). */
    public static final boolean DEBUG_CONFIGURATION = false;

    private static volatile ActivityThread sCurrentActivityThread;

    private final String mPackageName;
    private final String mProcessName;
    private final ClassLoader mClassLoader;
    private final ManifestPackage mPackage;
    private Application mInitialApplication;

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

    /** Whether this is system_server's thread: never in an app process. WindowConfiguration asks it. */
    public static boolean isSystem() {
        return false;
    }

    public static void main(String[] args) throws ClassNotFoundException, ErrnoException {
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
        sCurrentActivityThread = new ActivityThread(packageName, processName != null ? processName : packageName,
                loader, pkg);
        if (application != null) {
            // Loading and initializing the class runs its static initializer, as instantiating it does on Android.
            Class.forName(application, true, loader);
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
