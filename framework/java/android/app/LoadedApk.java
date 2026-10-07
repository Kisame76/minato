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
import java.util.ArrayList;

public final class LoadedApk {
    private final ActivityThread mActivityThread;
    final String mPackageName;
    private final ApplicationInfo mApplicationInfo;
    private final ClassLoader mClassLoader;
    private final AppComponentFactory mAppComponentFactory;
    private ApkAssets mApkAssets;
    // The overlays of the app's overlay paths, Monet's (ADR-0043 f)), loaded with its APK.
    private ApkAssets[] mOverlayAssets;
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
     * The app's Resources over base.apk, its overlays and the system's framework-res, with the system Resources'
     * configuration and display metrics, which the session gave them (b3.4c). Android's come from ResourcesManager,
     * whose display metrics ask DisplayManagerGlobal; that comes with ADR-0041's local display manager, and until then
     * a configuration change does not reach these Resources.
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
                mOverlayAssets = loadOverlays(mApplicationInfo.overlayPaths);
                Resources system = Resources.getSystem();
                mResources = new Resources(newAssetManager(), system.getDisplayMetrics(), system.getConfiguration());
            }
            return mResources;
        }
    }

    /**
     * As ResourcesManager.createAssetManager: the package's APK, then its overlays; AssetManager.Builder puts the
     * system's framework-res first.
     */
    private AssetManager newAssetManager() {
        AssetManager.Builder builder = new AssetManager.Builder().addApkAssets(mApkAssets);
        for (ApkAssets overlay : mOverlayAssets) {
            builder.addApkAssets(overlay);
        }
        return builder.build();
    }

    /**
     * The overlays of the app's overlay paths (ADR-0043 f)), each through the idmap that idmap2 keeps for it in
     * /data/resource-cache, as ResourcesManager.loadApkAssets loads an overlay; one that does not load is left out
     * with ResourcesManager's warning, as createAssetManager leaves it out.
     */
    private static ApkAssets[] loadOverlays(String[] overlayPaths) {
        ArrayList<ApkAssets> overlays = new ArrayList<>();
        if (overlayPaths != null) {
            for (String path : overlayPaths) {
                try {
                    overlays.add(ApkAssets.loadOverlayFromPath(
                            "/data/resource-cache/" + path.substring(1).replace('/', '@') + "@idmap", 0));
                } catch (IOException e) {
                    Slog.w("ResourcesManager", String.format("failed to add overlay path '%s'", path), e);
                }
            }
        }
        return overlays.toArray(new ApkAssets[0]);
    }

    /**
     * An activity's own Resources (step c2c3), as ResourcesManager.createBaseTokenResources gives each activity its
     * own: the package's APK and its overlays in an AssetManager of their own, the session's display metrics, and the
     * session's configuration with what the activity's window adds, its WindowConfiguration.
     */
    @SuppressWarnings("deprecation") // Resources' public constructor, as getResources.
    Resources createActivityResources(Configuration overrideConfiguration) {
        getResources();
        AssetManager assets = newAssetManager();
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
     * Makes the Application as makeApplicationInner does: the manifest's class for the process, else
     * android.app.Application, through the app's AppComponentFactory, attached to a new ContextImpl that then knows it
     * as its outer context, and what fails thrown with Android's message. `className`, ActivityThread.main's
     * --application, takes the manifest's place when it is not null (step c1.1).
     */
    public Application makeApplication(String className) {
        if (mApplication != null) {
            return mApplication;
        }
        String appClass = className != null ? className
                : mApplicationInfo.getCustomApplicationClassNameForProcess(mActivityThread.getProcessName());
        if (appClass == null) {
            appClass = "android.app.Application";
        }
        Application app;
        try {
            ContextImpl appContext = ContextImpl.createAppContext(mActivityThread, this);
            app = mAppComponentFactory.instantiateApplication(mClassLoader, appClass);
            app.attach(appContext);
            appContext.setOuterContext(app);
        } catch (Exception e) {
            throw new RuntimeException("Unable to instantiate application " + appClass + " package " + mPackageName
                    + ": " + e, e);
        }
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
