/*
 * The driver of the app process (ADR-0039 decision 5): our class under AOSP's name, which app_process starts with
 * main() on the guest main thread. Libraries reach it by reflection, so it keeps the tag's static getters.
 *
 * Step a's driver is empty: main() records the package and the process name, points TMPDIR and java.io.tmpdir at
 * the app's cache folder as handleBindApplication's setupGraphicsSupport does, builds the app's class loader over
 * base.apk and its lib/arm64, and loads and initializes the manifest's Application class, as makeApplication starts
 * on Android. It creates no Context and runs no Looper or lifecycle; those come in step c.
 *
 *   ActivityThread.main --package <package> --process <name> --apk <path of base.apk>
 *                       --library-path <lib/arm64 beside it> --data-dir <the app's data folder>
 *                       [--application <class>]
 */
package android.app;

import android.system.ErrnoException;
import android.system.Os;
import dalvik.system.PathClassLoader;

public final class ActivityThread {
    private static volatile ActivityThread sCurrentActivityThread;

    private final String mPackageName;
    private final String mProcessName;
    private final ClassLoader mClassLoader;
    private Application mInitialApplication;

    private ActivityThread(String packageName, String processName, ClassLoader classLoader) {
        mPackageName = packageName;
        mProcessName = processName;
        mClassLoader = classLoader;
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
        for (int i = 0; i + 1 < args.length; i += 2) {
            switch (args[i]) {
                case "--package": packageName = args[i + 1]; break;
                case "--process": processName = args[i + 1]; break;
                case "--apk": apk = args[i + 1]; break;
                case "--library-path": libraryPath = args[i + 1]; break;
                case "--application": application = args[i + 1]; break;
                case "--data-dir": dataDir = args[i + 1]; break;
                default: throw new IllegalArgumentException("ActivityThread.main: unknown argument " + args[i]);
            }
        }
        if (packageName == null || apk == null || libraryPath == null || dataDir == null || args.length % 2 != 0) {
            throw new IllegalArgumentException(
                    "usage: ActivityThread.main --package <package> [--process <name>] --apk <base.apk> "
                    + "--library-path <folder> --data-dir <folder> [--application <class>]");
        }
        // Context.getCacheDir's folder, which installd creates at the install.
        String tmpdir = dataDir + "/cache";
        System.setProperty("java.io.tmpdir", tmpdir);
        Os.setenv("TMPDIR", tmpdir, true);
        ClassLoader loader = new PathClassLoader(apk, libraryPath, ClassLoader.getSystemClassLoader());
        sCurrentActivityThread = new ActivityThread(packageName, processName != null ? processName : packageName,
                loader);
        if (application != null) {
            // Loading and initializing the class runs its static initializer, as instantiating it does on Android.
            Class.forName(application, true, loader);
        }
    }
}
