/*
 * The app's loaded package (ADR-0039 step c1): our class under AOSP's name, as decision 5 has it for ActivityThread.
 * Android's ActivityThread makes one from bindApplication's ApplicationInfo; it keeps the class loader, the app's
 * Resources and the Application, and makes the Application with its ContextImpl (makeApplicationInner). Ours does the
 * same for the one APK of the process, and since step c2c1 it makes the app's AppComponentFactory once, as
 * createAppFactory does, which then also makes the providers. Since ADR-0045's step d2 it keeps the registered
 * receivers' dispatchers, as AOSP's, through which the local activity manager service delivers broadcasts, and since
 * step SV3 the service connections' dispatchers. Android's also loads shared libraries, which come with M5.
 */
package android.app;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.StrictMode;
import android.util.ArrayMap;
import android.util.Log;
import android.util.Slog;
import dalvik.system.PathClassLoader;
import java.io.File;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;

public final class LoadedApk {
    static final String TAG = "LoadedApk";

    private final ActivityThread mActivityThread;
    final String mPackageName;
    private final ApplicationInfo mApplicationInfo;
    private final ClassLoader mClassLoader;
    private final AppComponentFactory mAppComponentFactory;
    private ApkAssets mApkAssets;
    // The splits' APKs after the base, in the order of splitSourceDirs.
    private ApkAssets[] mSplitAssets;
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
                String[] splitDirs = mApplicationInfo.splitSourceDirs != null
                        ? mApplicationInfo.splitSourceDirs : new String[0];
                mSplitAssets = new ApkAssets[splitDirs.length];
                for (int i = 0; i < splitDirs.length; i++) {
                    try {
                        mSplitAssets[i] = ApkAssets.loadFromPath(splitDirs[i]);
                    } catch (IOException e) {
                        throw new IllegalStateException("cannot load " + splitDirs[i] + "'s resources", e);
                    }
                }
                mOverlayAssets = loadOverlays(mApplicationInfo.overlayPaths);
                Resources system = Resources.getSystem();
                mResources = new Resources(newAssetManager(), system.getDisplayMetrics(), system.getConfiguration());
            }
            return mResources;
        }
    }

    /**
     * As ResourcesManager.createAssetManager: the package's APK, its splits, then its overlays;
     * AssetManager.Builder puts the system's framework-res first.
     */
    private AssetManager newAssetManager() {
        return newAssetManager(-1);
    }

    /**
     * The same for one split of an app that loads its splits in isolation (`split` its index, -1 for the app's own):
     * the base and the configuration splits of the base, then each split on the way from the base to `split` with the
     * configuration splits of each, as SplitAssetDependencyLoader makes them. An app that does not isolate its splits
     * has every split in all of them.
     */
    private AssetManager newAssetManager(int split) {
        AssetManager.Builder builder = new AssetManager.Builder().addApkAssets(mApkAssets);
        if (!isolated()) {
            for (ApkAssets apk : mSplitAssets) {
                builder.addApkAssets(apk);
            }
        } else {
            List<Integer> chain = new ArrayList<>();
            for (int i = split; i >= 0; i = parentOf(i)) {
                chain.add(0, i);
            }
            for (int i = 0; i < mSplitAssets.length; i++) {
                if (!feature(i) && parentOf(i) == -1) {
                    builder.addApkAssets(mSplitAssets[i]);
                }
            }
            for (int member : chain) {
                builder.addApkAssets(mSplitAssets[member]);
                for (int i = 0; i < mSplitAssets.length; i++) {
                    if (!feature(i) && parentOf(i) == member) {
                        builder.addApkAssets(mSplitAssets[i]);
                    }
                }
            }
        }
        for (ApkAssets overlay : mOverlayAssets) {
            builder.addApkAssets(overlay);
        }
        return builder.build();
    }

    // Isolated splits (ADR-0048 c): an app whose manifest asks for them has a class loader and Resources for each
    // split, whose parent is the split it depends on, else the base's.

    private boolean[] mFeatureSplits;
    private ClassLoader[] mSplitLoaders;

    /** Which splits, in splitNames' order, are feature splits; the rest configure one (ManifestPackage). */
    void setFeatureSplits(boolean[] featureSplits) {
        mFeatureSplits = featureSplits;
    }

    /** Whether the app loads its splits in isolation, as ApplicationInfo.requestsIsolatedSplitLoading says. */
    boolean isolated() {
        return mApplicationInfo.requestsIsolatedSplitLoading() && mApplicationInfo.splitNames != null
                && mApplicationInfo.splitDependencies != null;
    }

    private boolean feature(int split) {
        return mFeatureSplits != null && mFeatureSplits[split];
    }

    /** The index of the split `split` depends on, -1 for the base (ApplicationInfo.splitDependencies' parents). */
    private int parentOf(int split) {
        return mApplicationInfo.splitDependencies.get(split + 1)[0] - 1;
    }

    private int splitIndex(String splitName) throws PackageManager.NameNotFoundException {
        int index = splitName == null ? -1 : java.util.Arrays.asList(mApplicationInfo.splitNames).indexOf(splitName);
        if (splitName != null && index < 0) {
            throw new PackageManager.NameNotFoundException("Split name '" + splitName + "' is not installed");
        }
        return index;
    }

    /** As LoadedApk.getSplitClassLoader: the app's loader, or for an isolated split its own. */
    public ClassLoader getSplitClassLoader(String splitName) throws PackageManager.NameNotFoundException {
        return isolated() ? loaderOf(splitIndex(splitName)) : mClassLoader;
    }

    private ClassLoader loaderOf(int split) {
        if (split < 0) {
            return mClassLoader;
        }
        synchronized (this) {
            if (mSplitLoaders == null) {
                mSplitLoaders = new ClassLoader[mApplicationInfo.splitNames.length];
            }
            if (mSplitLoaders[split] == null) {
                mSplitLoaders[split] = new PathClassLoader(mApplicationInfo.splitSourceDirs[split],
                        mApplicationInfo.nativeLibraryDir, loaderOf(parentOf(split)));
            }
            return mSplitLoaders[split];
        }
    }

    /** The Resources of an isolated split, over the base and what it depends on; the app's own for any other app. */
    @SuppressWarnings("deprecation") // Resources' public constructor, as getResources.
    Resources getSplitResources(String splitName) throws PackageManager.NameNotFoundException {
        if (!isolated()) {
            return getResources();
        }
        int split = splitIndex(splitName);
        getResources();
        Resources system = Resources.getSystem();
        return new Resources(newAssetManager(split), system.getDisplayMetrics(), system.getConfiguration());
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
        return createActivityResources(overrideConfiguration, null);
    }

    /** The same for an activity of a split, which an isolated app gives that split's assets. */
    @SuppressWarnings("deprecation") // Resources' public constructor, as getResources.
    Resources createActivityResources(Configuration overrideConfiguration, String splitName) {
        getResources();
        int split = -1;
        try {
            split = isolated() ? splitIndex(splitName) : -1;
        } catch (PackageManager.NameNotFoundException e) {
            throw new IllegalStateException(e);
        }
        AssetManager assets = newAssetManager(split);
        Resources system = Resources.getSystem();
        Configuration config = new Configuration(system.getConfiguration());
        if (overrideConfiguration != null) {
            config.updateFrom(overrideConfiguration);
        }
        return new Resources(assets, system.getDisplayMetrics(), config);
    }

    /**
     * The Resources ApplicationPackageManager.getResourcesForApplication gives for the app's own package, as
     * ResourcesManager's getResources without an activity token makes them (the package manager stub plan, P1): a new
     * Resources at each call; without a configuration it shares getResources()' AssetManager, with one it has an
     * AssetManager of its own and the session's configuration updated from it, as an activity's
     * [verified: package-answers-test on apk-runner-35].
     */
    @SuppressWarnings("deprecation") // Resources' public constructor, as getResources.
    Resources createApplicationResources(Configuration overrideConfiguration) {
        if (overrideConfiguration != null) {
            return createActivityResources(overrideConfiguration);
        }
        Resources base = getResources();
        return new Resources(base.getAssets(), base.getDisplayMetrics(), base.getConfiguration());
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

    /** The registered receivers' dispatchers by the context that registered them (ADR-0045 decision 5). */
    private final ArrayMap<Context, ArrayMap<BroadcastReceiver, ReceiverDispatcher>> mReceivers = new ArrayMap<>();

    /**
     * As AOSP's: the receivers that `context` registered and did not unregister go when it goes, an activity's at its
     * destroy, each with ActivityThread's IntentReceiverLeaked line that shows where it was registered, and StrictMode's
     * report when its VM policy detects leaked registrations.
     */
    public void removeContextRegistrations(Context context, String who, String what) {
        final boolean reportRegistrationLeaks = StrictMode.vmRegistrationLeaksEnabled();
        synchronized (mReceivers) {
            ArrayMap<BroadcastReceiver, ReceiverDispatcher> rmap = mReceivers.remove(context);
            if (rmap != null) {
                for (int i = 0; i < rmap.size(); i++) {
                    ReceiverDispatcher rd = rmap.valueAt(i);
                    IntentReceiverLeaked leak = new IntentReceiverLeaked(what + " " + who + " has leaked IntentReceiver "
                            + rd.getIntentReceiver() + " that was originally registered here. Are you missing a "
                            + "call to unregisterReceiver()?");
                    leak.setStackTrace(rd.getLocation().getStackTrace());
                    Slog.e(ActivityThread.TAG, leak.getMessage(), leak);
                    if (reportRegistrationLeaks) {
                        StrictMode.onIntentReceiverLeaked(leak);
                    }
                    try {
                        ActivityManager.getService().unregisterReceiver(rd.getIIntentReceiver());
                    } catch (RemoteException e) {
                        throw e.rethrowFromSystemServer();
                    }
                }
            }
        }
        // Step SV3: the context's service connections, as AOSP's.
        synchronized (mServices) {
            ArrayMap<ServiceConnection, ServiceDispatcher> smap = mServices.remove(context);
            if (smap != null) {
                for (int i = 0; i < smap.size(); i++) {
                    ServiceDispatcher sd = smap.valueAt(i);
                    ServiceConnectionLeaked leak = new ServiceConnectionLeaked(what + " " + who
                            + " has leaked ServiceConnection " + sd.getServiceConnection()
                            + " that was originally bound here");
                    leak.setStackTrace(sd.getLocation().getStackTrace());
                    Slog.e(ActivityThread.TAG, leak.getMessage(), leak);
                    if (reportRegistrationLeaks) {
                        StrictMode.onServiceConnectionLeaked(leak);
                    }
                    try {
                        ActivityManager.getService().unbindService(sd.getIServiceConnection());
                    } catch (RemoteException e) {
                        throw e.rethrowFromSystemServer();
                    }
                    sd.doForget();
                }
            }
            mUnboundServices.remove(context);
        }
    }

    /** As AOSP's: the dispatcher of `r`, a registered receiver's kept per context until it is forgotten. */
    public IIntentReceiver getReceiverDispatcher(BroadcastReceiver r, Context context, Handler handler,
            Instrumentation instrumentation, boolean registered) {
        synchronized (mReceivers) {
            ReceiverDispatcher rd = null;
            ArrayMap<BroadcastReceiver, ReceiverDispatcher> map = null;
            if (registered) {
                map = mReceivers.get(context);
                if (map != null) {
                    rd = map.get(r);
                }
            }
            if (rd == null) {
                rd = new ReceiverDispatcher(mActivityThread.getApplicationThread(), r, context, handler,
                        instrumentation, registered);
                if (registered) {
                    if (map == null) {
                        map = new ArrayMap<>();
                        mReceivers.put(context, map);
                    }
                    map.put(r, rd);
                }
            } else {
                rd.validate(context, handler);
            }
            rd.mForgotten = false;
            return rd.getIIntentReceiver();
        }
    }

    /**
     * As AOSP's: forgets the dispatcher of a receiver `context` registered, and throws IllegalArgumentException for
     * one it did not; Android's debugUnregister, which keeps where it was unregistered, is not kept.
     */
    public IIntentReceiver forgetReceiverDispatcher(Context context, BroadcastReceiver r) {
        synchronized (mReceivers) {
            ArrayMap<BroadcastReceiver, ReceiverDispatcher> map = mReceivers.get(context);
            if (map != null) {
                ReceiverDispatcher rd = map.get(r);
                if (rd != null) {
                    map.remove(r);
                    if (map.size() == 0) {
                        mReceivers.remove(context);
                    }
                    rd.mForgotten = true;
                    return rd.getIIntentReceiver();
                }
            }
            if (context == null) {
                throw new IllegalStateException("Unbinding Receiver " + r + " from Context that is no longer in use: "
                        + context);
            }
            throw new IllegalArgumentException("Receiver not registered: " + r);
        }
    }

    /**
     * AOSP's receiver dispatcher: the service delivers through its InnerReceiver, whose Args runs onReceive on the
     * receiver's handler with itself as the PendingResult, so that an ordered broadcast's finish, also goAsync's from
     * another thread, sends the result back and lets the next receiver have it.
     */
    static final class ReceiverDispatcher {

        static final class InnerReceiver extends IIntentReceiver.Stub {
            final IApplicationThread mApplicationThread;
            final WeakReference<ReceiverDispatcher> mDispatcher;
            final ReceiverDispatcher mStrongRef;

            InnerReceiver(IApplicationThread thread, ReceiverDispatcher rd, boolean strong) {
                mApplicationThread = thread;
                mDispatcher = new WeakReference<>(rd);
                mStrongRef = strong ? rd : null;
            }

            @Override
            public void performReceive(Intent intent, int resultCode, String data, Bundle extras, boolean ordered,
                    boolean sticky, int sendingUser) {
                Log.wtf(TAG, "performReceive() called targeting raw IIntentReceiver for " + intent);
                performReceive(intent, resultCode, data, extras, ordered, sticky,
                        BroadcastReceiver.PendingResult.guessAssumeDelivered(
                                BroadcastReceiver.PendingResult.TYPE_REGISTERED, ordered),
                        sendingUser, Process.INVALID_UID, null);
            }

            public void performReceive(Intent intent, int resultCode, String data, Bundle extras, boolean ordered,
                    boolean sticky, boolean assumeDelivered, int sendingUser, int sendingUid, String sendingPackage) {
                final ReceiverDispatcher rd;
                if (intent == null) {
                    Log.wtf(TAG, "Null intent received");
                    rd = null;
                } else {
                    rd = mDispatcher.get();
                }
                if (rd != null) {
                    rd.performReceive(intent, resultCode, data, extras, ordered, sticky, assumeDelivered, sendingUser,
                            sendingUid, sendingPackage);
                } else if (!assumeDelivered) {
                    // The receiver went before its broadcast came: acknowledged for it, so that the next one gets it.
                    IActivityManager mgr = ActivityManager.getService();
                    try {
                        if (extras != null) {
                            extras.setAllowFds(false);
                        }
                        mgr.finishReceiver(mApplicationThread.asBinder(), resultCode, data, extras, false,
                                intent.getFlags());
                    } catch (RemoteException e) {
                        throw e.rethrowFromSystemServer();
                    }
                }
            }
        }

        final IApplicationThread mAppThread;
        final IIntentReceiver.Stub mIIntentReceiver;
        final BroadcastReceiver mReceiver;
        final Context mContext;
        final Handler mActivityThread;
        final Instrumentation mInstrumentation;
        final boolean mRegistered;
        final IntentReceiverLeaked mLocation;
        boolean mForgotten;

        final class Args extends BroadcastReceiver.PendingResult {
            private Intent mCurIntent;

            Args(Intent intent, int resultCode, String resultData, Bundle resultExtras, boolean ordered,
                    boolean sticky, boolean assumeDelivered, int sendingUser, int sendingUid, String sendingPackage) {
                super(resultCode, resultData, resultExtras, mRegistered ? TYPE_REGISTERED : TYPE_UNREGISTERED, ordered,
                        sticky, assumeDelivered, mAppThread.asBinder(), sendingUser, intent.getFlags(), sendingUid,
                        sendingPackage);
                mCurIntent = intent;
            }

            Runnable getRunnable() {
                return () -> {
                    final BroadcastReceiver receiver = mReceiver;
                    final IActivityManager mgr = ActivityManager.getService();
                    final Intent intent = mCurIntent;
                    if (intent == null) {
                        Log.wtf(TAG, "Null intent being dispatched");
                    }
                    mCurIntent = null;
                    if (receiver == null || intent == null || mForgotten) {
                        sendFinished(mgr);
                        return;
                    }
                    try {
                        ClassLoader cl = mReceiver.getClass().getClassLoader();
                        intent.setExtrasClassLoader(cl);
                        intent.prepareToEnterProcess(ManifestPackage.isProtectedBroadcast(intent.getAction()), null);
                        setExtrasClassLoader(cl);
                        receiver.setPendingResult(this);
                        receiver.onReceive(mContext, intent);
                    } catch (Exception e) {
                        sendFinished(mgr);
                        if (mInstrumentation == null || !mInstrumentation.onException(mReceiver, e)) {
                            throw new RuntimeException("Error receiving broadcast " + intent + " in " + mReceiver, e);
                        }
                    }
                    if (receiver.getPendingResult() != null) {
                        finish();
                    }
                };
            }
        }

        ReceiverDispatcher(IApplicationThread appThread, BroadcastReceiver receiver, Context context,
                Handler activityThread, Instrumentation instrumentation, boolean registered) {
            if (activityThread == null) {
                throw new NullPointerException("Handler must not be null");
            }
            mAppThread = appThread;
            mIIntentReceiver = new InnerReceiver(mAppThread, this, !registered);
            mReceiver = receiver;
            mContext = context;
            mActivityThread = activityThread;
            mInstrumentation = instrumentation;
            mRegistered = registered;
            mLocation = new IntentReceiverLeaked(null);
            mLocation.fillInStackTrace();
        }

        void validate(Context context, Handler activityThread) {
            if (mContext != context) {
                throw new IllegalStateException("Receiver " + mReceiver + " registered with differing Context (was "
                        + mContext + " now " + context + ")");
            }
            if (mActivityThread != activityThread) {
                throw new IllegalStateException("Receiver " + mReceiver + " registered with differing handler (was "
                        + mActivityThread + " now " + activityThread + ")");
            }
        }

        IntentReceiverLeaked getLocation() {
            return mLocation;
        }

        BroadcastReceiver getIntentReceiver() {
            return mReceiver;
        }

        IIntentReceiver getIIntentReceiver() {
            return mIIntentReceiver;
        }

        void performReceive(Intent intent, int resultCode, String data, Bundle extras, boolean ordered,
                boolean sticky, boolean assumeDelivered, int sendingUser, int sendingUid, String sendingPackage) {
            final Args args = new Args(intent, resultCode, data, extras, ordered, sticky, assumeDelivered, sendingUser,
                    sendingUid, sendingPackage);
            if (intent == null) {
                Log.wtf(TAG, "Null intent received");
            }
            if (intent == null || !mActivityThread.post(args.getRunnable())) {
                args.sendFinished(ActivityManager.getService());
            }
        }
    }

    /**
     * AOSP's check that the package cache's paths are still the app info's, which RemoteViews makes before it
     * inflates views (ADR-0045 gap 1, batch 2): Minato's process has its one package, whose paths stay while it runs.
     *
     * @hide
     */
    public static void checkAndUpdateApkPaths(ApplicationInfo expectedAppInfo) {
    }
    // Step SV3 of ADR-0045: the service connections' dispatchers, as AOSP's at android-15.0.0_r1, through which the local
    // activity manager service (LocalActiveServices) tells a ServiceConnection of its binder, on the connection's
    // handler or executor; kept per context until unbound, for removeContextRegistrations.
    private final ArrayMap<Context, ArrayMap<ServiceConnection, ServiceDispatcher>> mServices = new ArrayMap<>();
    /** The dispatchers unbound with BIND_DEBUG_UNBIND, which name where a second unbind's first one was. */
    private final ArrayMap<Context, ArrayMap<ServiceConnection, ServiceDispatcher>> mUnboundServices = new ArrayMap<>();

    public final IServiceConnection getServiceDispatcher(ServiceConnection c, Context context, Handler handler,
            long flags) {
        return getServiceDispatcherCommon(c, context, handler, null, flags);
    }

    public final IServiceConnection getServiceDispatcher(ServiceConnection c, Context context, Executor executor,
            long flags) {
        return getServiceDispatcherCommon(c, context, null, executor, flags);
    }

    private IServiceConnection getServiceDispatcherCommon(ServiceConnection c, Context context, Handler handler,
            Executor executor, long flags) {
        synchronized (mServices) {
            ServiceDispatcher sd = null;
            ArrayMap<ServiceConnection, ServiceDispatcher> map = mServices.get(context);
            if (map != null) {
                sd = map.get(c);
            }
            if (sd == null) {
                if (executor != null) {
                    sd = new ServiceDispatcher(c, context, executor, flags);
                } else {
                    sd = new ServiceDispatcher(c, context, handler, flags);
                }
                if (map == null) {
                    map = new ArrayMap<>();
                    mServices.put(context, map);
                }
                map.put(c, sd);
            } else {
                sd.validate(context, handler, executor);
            }
            return sd.getIServiceConnection();
        }
    }

    public IServiceConnection lookupServiceDispatcher(ServiceConnection c, Context context) {
        synchronized (mServices) {
            ServiceDispatcher sd = null;
            ArrayMap<ServiceConnection, ServiceDispatcher> map = mServices.get(context);
            if (map != null) {
                sd = map.get(c);
            }
            return sd != null ? sd.getIServiceConnection() : null;
        }
    }

    public final IServiceConnection forgetServiceDispatcher(Context context, ServiceConnection c) {
        synchronized (mServices) {
            ArrayMap<ServiceConnection, ServiceDispatcher> map = mServices.get(context);
            ServiceDispatcher sd = null;
            if (map != null) {
                sd = map.get(c);
                if (sd != null) {
                    map.remove(c);
                    sd.doForget();
                    if (map.size() == 0) {
                        mServices.remove(context);
                    }
                    if ((sd.getFlags() & Context.BIND_DEBUG_UNBIND) != 0) {
                        ArrayMap<ServiceConnection, ServiceDispatcher> holder = mUnboundServices.get(context);
                        if (holder == null) {
                            holder = new ArrayMap<>();
                            mUnboundServices.put(context, holder);
                        }
                        RuntimeException ex = new IllegalArgumentException("Originally unbound here:");
                        ex.fillInStackTrace();
                        sd.setUnbindLocation(ex);
                        holder.put(c, sd);
                    }
                    return sd.getIServiceConnection();
                }
            }
            ArrayMap<ServiceConnection, ServiceDispatcher> holder = mUnboundServices.get(context);
            if (holder != null) {
                sd = holder.get(c);
                if (sd != null) {
                    RuntimeException ex = sd.getUnbindLocation();
                    throw new IllegalArgumentException("Unbinding Service " + c + " that was already unbound", ex);
                }
            }
            if (context == null) {
                throw new IllegalStateException("Unbinding Service " + c
                        + " from Context that is no longer in use: " + context);
            } else {
                throw new IllegalArgumentException("Service not registered: " + c);
            }
        }
    }

    /** As AOSP's: a ServiceConnection's binders by component, each watched for its death. */
    static final class ServiceDispatcher {
        private final ServiceDispatcher.InnerConnection mIServiceConnection;
        private final ServiceConnection mConnection;
        private final Context mContext;
        private final Handler mActivityThread;
        private final Executor mActivityExecutor;
        private final ServiceConnectionLeaked mLocation;
        private final long mFlags;
        private RuntimeException mUnbindLocation;
        private boolean mForgotten;

        private static class ConnectionInfo {
            IBinder binder;
            IBinder.DeathRecipient deathMonitor;
        }

        private static class InnerConnection extends IServiceConnection.Stub {
            final WeakReference<ServiceDispatcher> mDispatcher;

            InnerConnection(ServiceDispatcher sd) {
                mDispatcher = new WeakReference<>(sd);
            }

            @Override
            public void connected(ComponentName name, IBinder service, boolean dead) throws RemoteException {
                ServiceDispatcher sd = mDispatcher.get();
                if (sd != null) {
                    sd.connected(name, service, dead);
                }
            }
        }

        private final ArrayMap<ComponentName, ConnectionInfo> mActiveConnections = new ArrayMap<>();

        ServiceDispatcher(ServiceConnection conn, Context context, Handler activityThread, long flags) {
            mIServiceConnection = new InnerConnection(this);
            mConnection = conn;
            mContext = context;
            mActivityThread = activityThread;
            mActivityExecutor = null;
            mLocation = new ServiceConnectionLeaked(null);
            mLocation.fillInStackTrace();
            mFlags = flags;
        }

        ServiceDispatcher(ServiceConnection conn, Context context, Executor activityExecutor, long flags) {
            mIServiceConnection = new InnerConnection(this);
            mConnection = conn;
            mContext = context;
            mActivityThread = null;
            mActivityExecutor = activityExecutor;
            mLocation = new ServiceConnectionLeaked(null);
            mLocation.fillInStackTrace();
            mFlags = flags;
        }

        void validate(Context context, Handler activityThread, Executor activityExecutor) {
            if (mContext != context) {
                throw new RuntimeException("ServiceConnection " + mConnection
                        + " registered with differing Context (was " + mContext + " now " + context + ")");
            }
            if (mActivityThread != activityThread) {
                throw new RuntimeException("ServiceConnection " + mConnection
                        + " registered with differing handler (was " + mActivityThread + " now " + activityThread
                        + ")");
            }
            if (mActivityExecutor != activityExecutor) {
                throw new RuntimeException("ServiceConnection " + mConnection
                        + " registered with differing executor (was " + mActivityExecutor + " now "
                        + activityExecutor + ")");
            }
        }

        void doForget() {
            synchronized (this) {
                for (int i = 0; i < mActiveConnections.size(); i++) {
                    ConnectionInfo ci = mActiveConnections.valueAt(i);
                    ci.binder.unlinkToDeath(ci.deathMonitor, 0);
                }
                mActiveConnections.clear();
                mForgotten = true;
            }
        }

        ServiceConnectionLeaked getLocation() {
            return mLocation;
        }

        ServiceConnection getServiceConnection() {
            return mConnection;
        }

        IServiceConnection getIServiceConnection() {
            return mIServiceConnection;
        }

        long getFlags() {
            return mFlags;
        }

        void setUnbindLocation(RuntimeException ex) {
            mUnbindLocation = ex;
        }

        RuntimeException getUnbindLocation() {
            return mUnbindLocation;
        }

        public void connected(ComponentName name, IBinder service, boolean dead) {
            if (mActivityExecutor != null) {
                mActivityExecutor.execute(new RunConnection(name, service, 0, dead));
            } else if (mActivityThread != null) {
                mActivityThread.post(new RunConnection(name, service, 0, dead));
            } else {
                doConnected(name, service, dead);
            }
        }

        public void death(ComponentName name, IBinder service) {
            if (mActivityExecutor != null) {
                mActivityExecutor.execute(new RunConnection(name, service, 1, false));
            } else if (mActivityThread != null) {
                mActivityThread.post(new RunConnection(name, service, 1, false));
            } else {
                doDeath(name, service);
            }
        }

        public void doConnected(ComponentName name, IBinder service, boolean dead) {
            ConnectionInfo old;
            ConnectionInfo info;
            synchronized (this) {
                if (mForgotten) {
                    // Unbound before the connection came: it is ignored.
                    return;
                }
                old = mActiveConnections.get(name);
                if (old != null && old.binder == service) {
                    return;
                }
                if (service != null) {
                    info = new ConnectionInfo();
                    info.binder = service;
                    info.deathMonitor = new DeathMonitor(name, service);
                    try {
                        service.linkToDeath(info.deathMonitor, 0);
                        mActiveConnections.put(name, info);
                    } catch (RemoteException e) {
                        mActiveConnections.remove(name);
                        return;
                    }
                } else {
                    mActiveConnections.remove(name);
                }
                if (old != null) {
                    old.binder.unlinkToDeath(old.deathMonitor, 0);
                }
            }
            if (old != null) {
                mConnection.onServiceDisconnected(name);
            }
            if (dead) {
                mConnection.onBindingDied(name);
            } else if (service != null) {
                mConnection.onServiceConnected(name, service);
            } else {
                // The binding worked, and the service's onBind answered null.
                mConnection.onNullBinding(name);
            }
        }

        public void doDeath(ComponentName name, IBinder service) {
            synchronized (this) {
                ConnectionInfo old = mActiveConnections.get(name);
                if (old == null || old.binder != service) {
                    return;
                }
                mActiveConnections.remove(name);
                old.binder.unlinkToDeath(old.deathMonitor, 0);
            }
            mConnection.onServiceDisconnected(name);
        }

        private final class RunConnection implements Runnable {
            final ComponentName mName;
            final IBinder mService;
            final int mCommand;
            final boolean mDead;

            RunConnection(ComponentName name, IBinder service, int command, boolean dead) {
                mName = name;
                mService = service;
                mCommand = command;
                mDead = dead;
            }

            @Override
            public void run() {
                if (mCommand == 0) {
                    doConnected(mName, mService, mDead);
                } else if (mCommand == 1) {
                    doDeath(mName, mService);
                }
            }
        }

        private final class DeathMonitor implements IBinder.DeathRecipient {
            final ComponentName mName;
            final IBinder mService;

            DeathMonitor(ComponentName name, IBinder service) {
                mName = name;
                mService = service;
            }

            @Override
            public void binderDied() {
                death(mName, mService);
            }
        }
    }
}
