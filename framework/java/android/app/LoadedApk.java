/*
 * The app's loaded package (ADR-0039 step c1): our class under AOSP's name, as decision 5 has it for ActivityThread.
 * Android's ActivityThread makes one from bindApplication's ApplicationInfo; it keeps the class loader, the app's
 * Resources and the Application, and makes the Application with its ContextImpl (makeApplicationInner). Ours does the
 * same for the one APK of the process, and since step c2c1 it makes the app's AppComponentFactory once, as
 * createAppFactory does, which then also makes the providers. Android's also loads splits and shared libraries, and
 * dispatches receivers and services through system_server; those come with M5 and ADR-0045.
 */
package android.app;

import android.content.pm.ApplicationInfo;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.Slog;
import java.io.File;
import java.io.IOException;

public final class LoadedApk {
    private final ActivityThread mActivityThread;
    final String mPackageName;
    private final ApplicationInfo mApplicationInfo;
    private final ClassLoader mClassLoader;
    private final AppComponentFactory mAppComponentFactory;
    private ApkAssets mApkAssets;
    private Resources mResources;
    private Application mApplication;

    LoadedApk(ActivityThread activityThread, ApplicationInfo info, ClassLoader classLoader) {
        mActivityThread = activityThread;
        mPackageName = info.packageName;
        mApplicationInfo = info;
        mClassLoader = classLoader;
        mAppComponentFactory = createAppFactory(info, classLoader);
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
                try {
                    mApkAssets = ApkAssets.loadFromPath(getResDir());
                } catch (IOException e) {
                    throw new IllegalStateException("cannot load " + getResDir() + "'s resources", e);
                }
                AssetManager assets = new AssetManager.Builder().addApkAssets(mApkAssets).build();
                Resources system = Resources.getSystem();
                mResources = new Resources(assets, system.getDisplayMetrics(), system.getConfiguration());
            }
            return mResources;
        }
    }

    /**
     * An activity's own Resources (step c2c3), as ResourcesManager.createBaseTokenResources gives each activity its
     * own: the package's APK in an AssetManager of their own, the session's display metrics, and the session's
     * configuration with what the activity's window adds, its WindowConfiguration.
     */
    @SuppressWarnings("deprecation") // Resources' public constructor, as getResources.
    Resources createActivityResources(Configuration overrideConfiguration) {
        getResources();
        AssetManager assets = new AssetManager.Builder().addApkAssets(mApkAssets).build();
        Resources system = Resources.getSystem();
        Configuration config = new Configuration(system.getConfiguration());
        if (overrideConfiguration != null) {
            config.updateFrom(overrideConfiguration);
        }
        return new Resources(assets, system.getDisplayMetrics(), config);
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
        Application app = mAppComponentFactory.instantiateApplication(mClassLoader,
                className != null ? className : Application.class.getName());
        app.attach(appContext);
        appContext.setOuterContext(app);
        mApplication = app;
        return app;
    }

    /** The app's AppComponentFactory, through which the Application and the providers are made (step c2c1). */
    public AppComponentFactory getAppFactory() {
        return mAppComponentFactory;
    }

    /**
     * android:appComponentFactory's class, as createAppFactory makes it once for the package; one that cannot be
     * made is logged and the default takes its place, as on Android.
     */
    @SuppressWarnings("deprecation") // Class.newInstance, as AOSP's.
    private static AppComponentFactory createAppFactory(ApplicationInfo info, ClassLoader classLoader) {
        if (info.appComponentFactory != null && classLoader != null) {
            try {
                return (AppComponentFactory) classLoader.loadClass(info.appComponentFactory).newInstance();
            } catch (InstantiationException | IllegalAccessException | ClassNotFoundException e) {
                Slog.e("LoadedApk", "Unable to instantiate appComponentFactory", e);
            }
        }
        return AppComponentFactory.DEFAULT;
    }
}
