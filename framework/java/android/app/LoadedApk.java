/*
 * The app's loaded package (ADR-0039 step c1): our class under AOSP's name, as decision 5 has it for ActivityThread.
 * Android's ActivityThread makes one from bindApplication's ApplicationInfo; it keeps the class loader, the app's
 * Resources and the Application, and makes the Application with its ContextImpl (makeApplicationInner). Ours does the
 * same for the one APK of the process. Android's also loads splits and shared libraries, and dispatches receivers and
 * services through system_server; those come with M5 and ADR-0045.
 */
package android.app;

import android.content.pm.ApplicationInfo;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Resources;
import java.io.File;
import java.io.IOException;

public final class LoadedApk {
    private final ActivityThread mActivityThread;
    final String mPackageName;
    private final ApplicationInfo mApplicationInfo;
    private final ClassLoader mClassLoader;
    private Resources mResources;
    private Application mApplication;

    LoadedApk(ActivityThread activityThread, ApplicationInfo info, ClassLoader classLoader) {
        mActivityThread = activityThread;
        mPackageName = info.packageName;
        mApplicationInfo = info;
        mClassLoader = classLoader;
    }

    public String getPackageName() {
        return mPackageName;
    }

    public ApplicationInfo getApplicationInfo() {
        return mApplicationInfo;
    }

    public ClassLoader getClassLoader() {
        return mClassLoader;
    }

    /** base.apk's path, which Context.getPackageCodePath gives. */
    public String getAppDir() {
        return mApplicationInfo.sourceDir;
    }

    /** The resources' APK, base.apk too: Android takes publicSourceDir only for another uid's package. */
    public String getResDir() {
        return mApplicationInfo.sourceDir;
    }

    public File getDataDirFile() {
        return mApplicationInfo.dataDir != null ? new File(mApplicationInfo.dataDir) : null;
    }

    public File getDeviceProtectedDataDirFile() {
        return mApplicationInfo.deviceProtectedDataDir != null
                ? new File(mApplicationInfo.deviceProtectedDataDir) : null;
    }

    public File getCredentialProtectedDataDirFile() {
        return mApplicationInfo.credentialProtectedDataDir != null
                ? new File(mApplicationInfo.credentialProtectedDataDir) : null;
    }

    /**
     * The app's Resources over base.apk and the system's framework-res, with the system Resources' configuration
     * and display metrics, which the session gave them (b3.4c). Android's come from ResourcesManager, whose display
     * metrics ask DisplayManagerGlobal; that comes with ADR-0041's local display manager, and until then a
     * configuration change does not reach these Resources.
     */
    @SuppressWarnings("deprecation") // Resources' public constructor, which ResourcesManager's path replaces.
    public Resources getResources() {
        synchronized (this) {
            if (mResources == null) {
                ApkAssets apk;
                try {
                    apk = ApkAssets.loadFromPath(getResDir());
                } catch (IOException e) {
                    throw new IllegalStateException("cannot load " + getResDir() + "'s resources", e);
                }
                AssetManager assets = new AssetManager.Builder().addApkAssets(apk).build();
                Resources system = Resources.getSystem();
                mResources = new Resources(assets, system.getDisplayMetrics(), system.getConfiguration());
            }
            return mResources;
        }
    }

    /** The Application once makeApplication made it, else null: also while its attachBaseContext runs. */
    public Application getApplication() {
        return mApplication;
    }

    /**
     * Makes the Application as makeApplicationInner does: the manifest's class, else android.app.Application,
     * through the app's AppComponentFactory, attached to a new ContextImpl that then knows it as its outer context.
     */
    public Application makeApplication(String className) throws ReflectiveOperationException {
        if (mApplication != null) {
            return mApplication;
        }
        ContextImpl appContext = ContextImpl.createAppContext(mActivityThread, this);
        Application app = appComponentFactory().instantiateApplication(mClassLoader,
                className != null ? className : Application.class.getName());
        app.attach(appContext);
        appContext.setOuterContext(app);
        mApplication = app;
        return app;
    }

    /** android:appComponentFactory's class, as createAppFactory makes it, else the default. */
    private AppComponentFactory appComponentFactory() throws ReflectiveOperationException {
        String factory = mApplicationInfo.appComponentFactory;
        return factory != null
                ? (AppComponentFactory) mClassLoader.loadClass(factory).getDeclaredConstructor().newInstance()
                : AppComponentFactory.DEFAULT;
    }
}
