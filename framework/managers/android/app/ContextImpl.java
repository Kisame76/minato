// The hand file of our ContextImpl (ADR-0039 step c1, under AOSP's name as decision 5 has it for ActivityThread):
// tools/managergen --implement writes the class from Context's abstract methods, and the members here replace the
// generated ones, which log once and return their default. It answers from the app's LoadedApk without
// system_server: decision 6's folders and files with the modes Android's ContextImpl gives them, the resources, the
// class loader, the package and its PackageManager, SharedPreferences through AOSP's SharedPreferencesImpl, and
// permission checks from the manifest's grants. The paths and SharedPreferences follow the tag's ContextImpl.java, the
// external folders through Environment's one emulated volume (framework/patches). Step c2c2 adds the theme and the
// LayoutInflater, as the tag's ContextImpl and SystemServiceRegistry give them, so that a view can be made. Step c2c3
// adds an activity's context, with its token and its own Resources, the window manager and the display: an
// activity's context is a UI context associated with display 0, the application's is not, as on Android. ADR-0045's
// gap 13a adds the ContentResolver, the tag's ApplicationContentResolver over ActivityThread.acquireProvider.
import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.Context;
import android.content.ContextParams;
import android.content.IContentProvider;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.database.DatabaseErrorHandler;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteDatabase.CursorFactory;
import android.hardware.SensorManager;
import android.hardware.SystemSensorManager;
import android.hardware.input.InputManager;
import android.net.wifi.WifiManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.FileUtils;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.RemoteException;
import android.os.UserHandle;
import android.os.UserManager;
import android.os.storage.StorageManager;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.AndroidRuntimeException;
import android.util.ArrayMap;
import android.util.Log;
import android.view.Display;
import android.view.LayoutInflater;
import android.view.WindowManager;
import android.view.WindowManagerImpl;
import android.view.inputmethod.InputMethodManager;
import com.android.internal.policy.PhoneLayoutInflater;
import com.android.minato.services.Unwritten;
import dalvik.system.BlockGuard;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import libcore.io.Memory;

class ContextImpl {
    private static final String TAG = "ContextImpl";
    private static final String XATTR_INODE_CACHE = "user.inode_cache";
    private static final String XATTR_INODE_CODE_CACHE = "user.inode_code_cache";

    /** @hide */
    final ActivityThread mMainThread;
    /** @hide */
    final LoadedApk mPackageInfo;
    private final String mOpPackageName;
    private final ApplicationContentResolver mContentResolver;
    private final Object mSync = new Object();
    private Context mOuterContext = (Context) (Object) this;
    private PackageManager mPackageManager;
    private File mFilesDir;
    private File mNoBackupFilesDir;
    private File mCacheDir;
    private File mCodeCacheDir;
    private File mDatabasesDir;
    private File mPreferencesDir;
    /** Each package's SharedPreferences by file, shared by its contexts, as the tag keeps them. */
    private static ArrayMap<String, ArrayMap<File, SharedPreferencesImpl>> sSharedPrefsCache;
    private ArrayMap<String, File> mSharedPrefsPaths;
    /** The theme's resource, 0 until setTheme or getTheme picks one. */
    private int mThemeResource = 0;
    private Resources.Theme mTheme;
    /** @hide The services made for this context, by SystemServiceRegistry's index, as the tag's mServiceCache. */
    final Object[] mServiceCache = SystemServiceRegistry.createServiceCache();
    /** An activity context's token and its own Resources; the application's has neither. */
    private IBinder mToken;
    private boolean mIsActivityContext;
    private Resources mResources;
    /** A context that createDisplayContext made, which is associated with display 0 without being a UI context. */
    private boolean mIsDisplayContext;
    /** The storage flags of the tag's mFlags: credential or device protected. */
    private final int mFlags;
    /** The attribution tag of createAttributionContext, null for the app's default. */
    private String mAttributionTag;
    /** The class loader of an isolated split's context, null for the package's. */
    private ClassLoader mClassLoader;

    private ContextImpl(ActivityThread mainThread, LoadedApk packageInfo) {
        this(mainThread, packageInfo, 0);
    }

    /**
     * As the tag's constructor: a context whose flags name no storage takes the one of its package's data folder, so
     * that an app's own context is credential protected.
     */
    private ContextImpl(ActivityThread mainThread, LoadedApk packageInfo, int flags) {
        if ((flags & (Context.CONTEXT_CREDENTIAL_PROTECTED_STORAGE | Context.CONTEXT_DEVICE_PROTECTED_STORAGE)) == 0) {
            File dataDir = packageInfo.getDataDirFile();
            if (Objects.equals(dataDir, packageInfo.getCredentialProtectedDataDirFile())) {
                flags |= Context.CONTEXT_CREDENTIAL_PROTECTED_STORAGE;
            } else if (Objects.equals(dataDir, packageInfo.getDeviceProtectedDataDirFile())) {
                flags |= Context.CONTEXT_DEVICE_PROTECTED_STORAGE;
            }
        }
        mFlags = flags;
        mMainThread = mainThread;
        mPackageInfo = packageInfo;
        mOpPackageName = packageInfo.getPackageName();
        mContentResolver = new ApplicationContentResolver((Context) (Object) this, mainThread);
    }

    /** As the tag's constructor with a container: this context's token, Resources and kind, with `flags`. */
    private ContextImpl copy(int flags) {
        ContextImpl context = new ContextImpl(mMainThread, mPackageInfo, flags);
        context.mToken = mToken;
        context.mIsActivityContext = mIsActivityContext;
        context.mIsDisplayContext = mIsDisplayContext;
        context.mResources = mResources;
        context.mAttributionTag = mAttributionTag;
        context.mClassLoader = mClassLoader;
        return context;
    }

    /**
     * The application's context, which LoadedApk.makeApplication makes before the Application.
     *
     * @hide
     */
    static ContextImpl createAppContext(ActivityThread mainThread, LoadedApk packageInfo) {
        return new ContextImpl(mainThread, packageInfo);
    }

    /**
     * The ContextImpl under a chain of wrappers, as AOSP's Application.attach looks for it.
     *
     * @hide
     */
    static ContextImpl getImpl(Context context) {
        Context next;
        while (context instanceof android.content.ContextWrapper
                && (next = ((android.content.ContextWrapper) context).getBaseContext()) != null) {
            context = next;
        }
        return (ContextImpl) context;
    }

    /**
     * An activity's context, as ActivityThread.createBaseContextForActivity asks for it: the activity's token, and
     * Resources of its own with the window's configuration (step c2c3).
     *
     * @hide
     */
    static ContextImpl createActivityContext(ActivityThread mainThread, LoadedApk packageInfo,
            ActivityInfo activityInfo, IBinder activityToken, int displayId, Configuration overrideConfiguration) {
        ContextImpl context = new ContextImpl(mainThread, packageInfo);
        context.mToken = activityToken;
        context.mIsActivityContext = true;
        context.mResources = packageInfo.createActivityResources(overrideConfiguration, activityInfo.splitName);
        // An activity of an isolated split is made by that split's loader (the tag's createActivityContext).
        if (packageInfo.isolated() && activityInfo.splitName != null) {
            try {
                context.mClassLoader = packageInfo.getSplitClassLoader(activityInfo.splitName);
            } catch (PackageManager.NameNotFoundException e) {
                throw new IllegalStateException(e);
            }
        }
        return context;
    }

    /** @hide */
    final void setOuterContext(Context context) {
        mOuterContext = context;
    }

    /** @hide */
    final Context getOuterContext() {
        return mOuterContext;
    }

    // The package, its code and its resources.

    public AssetManager getAssets() {
        return getResources().getAssets();
    }

    public Resources getResources() {
        return mResources != null ? mResources : mPackageInfo.getResources();
    }

    // The theme and the services (step c2c2).

    public void setTheme(int resId) {
        synchronized (mSync) {
            if (mThemeResource != resId) {
                mThemeResource = resId;
                initializeTheme();
            }
        }
    }

    /** @hide */
    public int getThemeResId() {
        synchronized (mSync) {
            return mThemeResource;
        }
    }

    /** The theme, the platform's default for the app's target SDK until setTheme picks one, as the tag's. */
    public Resources.Theme getTheme() {
        synchronized (mSync) {
            if (mTheme != null) {
                return mTheme;
            }
            mThemeResource = Resources.selectDefaultTheme(mThemeResource,
                    getOuterContext().getApplicationInfo().targetSdkVersion);
            initializeTheme();
            return mTheme;
        }
    }

    private void initializeTheme() {
        if (mTheme == null) {
            mTheme = getResources().newTheme();
        }
        mTheme.applyStyle(mThemeResource, true);
    }

    /**
     * The context's service of {@code name}, from SystemServiceRegistry (ADR-0045 decision 2, gap 1), made once per
     * context as the tag's CachedServiceFetcher keeps them; a name without one is logged once and gives null.
     */
    public Object getSystemService(String name) {
        return SystemServiceRegistry.getSystemService(this, name);
    }

    /** The default device's, as an app's context on a phone has it: Minato has no virtual devices. */
    public int getDeviceId() {
        return Context.DEVICE_ID_DEFAULT;
    }

    /** The display a context without one has, as the tag's answers: the default display. */
    public int getDisplayId() {
        return android.view.Display.DEFAULT_DISPLAY;
    }

    /** Display 0, adjusted to the context's Resources as the tag's asks ResourcesManager (step c2c3). */
    public Display getDisplayNoVerify() {
        return ResourcesManager.getInstance().getAdjustedDisplay(getDisplayId(), getResources());
    }

    /** As the tag's: only a context associated with a display, an activity's or a display context, has one to give. */
    public Display getDisplay() {
        if (!mIsActivityContext && !mIsDisplayContext) {
            throw new UnsupportedOperationException("Tried to obtain display from a Context not associated with one."
                    + " Only visual Contexts (such as Activity or one created with Context#createWindowContext) or"
                    + " ones created with Context#createDisplayContext are associated with displays. Other types of"
                    + " Contexts are typically related to background entities and may return an arbitrary display.");
        }
        return getDisplayNoVerify();
    }

    /** @hide */
    public int getAssociatedDisplayId() {
        return mIsActivityContext || mIsDisplayContext ? getDisplayId() : Display.INVALID_DISPLAY;
    }

    /** @hide An activity's context is a UI context, the application's is not. */
    public boolean isUiContext() {
        return mIsActivityContext;
    }

    /** @hide */
    public IBinder getActivityToken() {
        return mIsActivityContext ? mToken : null;
    }

    /** @hide No context of Minato's is a window context (createWindowContext). */
    public IBinder getWindowContextToken() {
        return null;
    }

    public String getSystemServiceName(Class<?> serviceClass) {
        return SystemServiceRegistry.getSystemServiceName(serviceClass);
    }

    public PackageManager getPackageManager() {
        synchronized (mSync) {
            if (mPackageManager == null) {
                mPackageManager = new ApplicationPackageManager(mMainThread);
            }
            return mPackageManager;
        }
    }

    public ContentResolver getContentResolver() {
        return mContentResolver;
    }

    // ADR-0045 decision 5, step d2: receivers and broadcasts within the process, as the tag's ContextImpl has them,
    // through LoadedApk's dispatchers and the local activity manager service. Sticky broadcasts, other users and the
    // receiver's app ops stay the generated methods.

    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) {
        return registerReceiver(receiver, filter, null, null);
    }

    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, int flags) {
        return registerReceiver(receiver, filter, null, null, flags);
    }

    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission,
            Handler scheduler) {
        return registerReceiverInternal(receiver, filter, broadcastPermission, scheduler, getOuterContext(), 0);
    }

    public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission,
            Handler scheduler, int flags) {
        return registerReceiverInternal(receiver, filter, broadcastPermission, scheduler, getOuterContext(), flags);
    }

    /** As AOSP's: the receiver's dispatcher on its handler, the main thread's by default, then the service. */
    private Intent registerReceiverInternal(BroadcastReceiver receiver, IntentFilter filter, String broadcastPermission,
            Handler scheduler, Context context, int flags) {
        IIntentReceiver rd = null;
        if (receiver != null) {
            rd = mPackageInfo.getReceiverDispatcher(receiver, context,
                    scheduler != null ? scheduler : mMainThread.getHandler(), mMainThread.getInstrumentation(), true);
        }
        try {
            final Intent intent = ActivityManager.getService().registerReceiverWithFeature(
                    mMainThread.getApplicationThread(), getBasePackageName(), null, null, rd, filter,
                    broadcastPermission, UserHandle.myUserId(), flags);
            if (intent != null) {
                intent.setExtrasClassLoader(getClassLoader());
                intent.prepareToEnterProcess(ManifestPackage.isProtectedBroadcast(intent.getAction()), null);
            }
            return intent;
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public void unregisterReceiver(BroadcastReceiver receiver) {
        IIntentReceiver rd = mPackageInfo.forgetReceiverDispatcher(getOuterContext(), receiver);
        try {
            ActivityManager.getService().unregisterReceiver(rd);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public void sendBroadcast(Intent intent) {
        broadcast(intent, null, null, false, null, null, Activity.RESULT_OK, null, null);
    }

    public void sendBroadcast(Intent intent, String receiverPermission) {
        broadcast(intent, permissions(receiverPermission), null, false, null, null, Activity.RESULT_OK, null, null);
    }

    public void sendBroadcast(Intent intent, String receiverPermission, Bundle options) {
        broadcast(intent, permissions(receiverPermission), options, false, null, null, Activity.RESULT_OK, null, null);
    }

    public void sendBroadcastWithMultiplePermissions(Intent intent, String[] receiverPermissions) {
        broadcast(intent, receiverPermissions, null, false, null, null, Activity.RESULT_OK, null, null);
    }

    public void sendOrderedBroadcast(Intent intent, String receiverPermission) {
        broadcast(intent, permissions(receiverPermission), null, true, null, null, Activity.RESULT_OK, null, null);
    }

    public void sendOrderedBroadcast(Intent intent, String receiverPermission, Bundle options) {
        broadcast(intent, permissions(receiverPermission), options, true, null, null, Activity.RESULT_OK, null, null);
    }

    public void sendOrderedBroadcast(Intent intent, String receiverPermission, BroadcastReceiver resultReceiver,
            Handler scheduler, int initialCode, String initialData, Bundle initialExtras) {
        broadcast(intent, permissions(receiverPermission), null, true, resultReceiver, scheduler, initialCode,
                initialData, initialExtras);
    }

    public void sendOrderedBroadcast(Intent intent, String receiverPermission, Bundle options,
            BroadcastReceiver resultReceiver, Handler scheduler, int initialCode, String initialData,
            Bundle initialExtras) {
        broadcast(intent, permissions(receiverPermission), options, true, resultReceiver, scheduler, initialCode,
                initialData, initialExtras);
    }

    /** With an app op, which the app's own receivers are not asked about (the generated AppOpsManager answers). */
    public void sendOrderedBroadcast(Intent intent, String receiverPermission, String receiverAppOp,
            BroadcastReceiver resultReceiver, Handler scheduler, int initialCode, String initialData,
            Bundle initialExtras) {
        broadcast(intent, permissions(receiverPermission), null, true, resultReceiver, scheduler, initialCode,
                initialData, initialExtras);
    }

    private static String[] permissions(String receiverPermission) {
        return receiverPermission == null ? null : new String[] {receiverPermission};
    }

    /**
     * As AOSP's sendBroadcast and sendOrderedBroadcastAsUser: the final receiver's dispatcher, unregistered, on its
     * handler, the main thread's by default; the intent's resolved type; then the service.
     */
    private void broadcast(Intent intent, String[] receiverPermissions, Bundle options, boolean ordered,
            BroadcastReceiver resultReceiver, Handler scheduler, int initialCode, String initialData,
            Bundle initialExtras) {
        IIntentReceiver rd = null;
        if (resultReceiver != null) {
            rd = mPackageInfo.getReceiverDispatcher(resultReceiver, getOuterContext(),
                    scheduler != null ? scheduler : mMainThread.getHandler(), mMainThread.getInstrumentation(), false);
        }
        String resolvedType = intent.resolveTypeIfNeeded(getContentResolver());
        try {
            intent.prepareToLeaveProcess(getOuterContext());
            ActivityManager.getService().broadcastIntentWithFeature(mMainThread.getApplicationThread(), null, intent,
                    resolvedType, rd, initialCode, initialData, initialExtras, receiverPermissions, null, null,
                    AppOpsManager.OP_NONE, options, ordered, false, UserHandle.myUserId());
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    /** @hide As AOSP's: the context's registrations go after the message that ends it, an activity's destroy. */
    final void scheduleFinalCleanup(String who, String what) {
        mMainThread.getHandler().post(() -> performFinalCleanup(who, what));
    }

    /** @hide */
    final void performFinalCleanup(String who, String what) {
        mPackageInfo.removeContextRegistrations(getOuterContext(), who, what);
    }

    /** As the tag's: the main thread's Looper. */
    public Looper getMainLooper() {
        return mMainThread.getLooper();
    }

    /** The Application, once LoadedApk made it: null while its attachBaseContext runs, as on Android. */
    public Context getApplicationContext() {
        return mPackageInfo.getApplication();
    }

    public ClassLoader getClassLoader() {
        return mClassLoader != null ? mClassLoader : mPackageInfo.getClassLoader();
    }

    public String getPackageName() {
        return mPackageInfo.getPackageName();
    }

    public String getBasePackageName() {
        return mPackageInfo.getPackageName();
    }

    public String getOpPackageName() {
        return mOpPackageName;
    }

    public ApplicationInfo getApplicationInfo() {
        return mPackageInfo.getApplicationInfo();
    }

    public String getPackageResourcePath() {
        return mPackageInfo.getResDir();
    }

    public String getPackageCodePath() {
        return mPackageInfo.getAppDir();
    }

    // Decision 6's folders, made with the modes of the tag's ensurePrivateDirExists and getDir.

    /** As the tag's: the folder of the context's storage, /data/user/0/<package> or /data/user_de/0/<package>. */
    public File getDataDir() {
        File dataDir;
        if (isCredentialProtectedStorage()) {
            dataDir = mPackageInfo.getCredentialProtectedDataDirFile();
        } else if (isDeviceProtectedStorage()) {
            dataDir = mPackageInfo.getDeviceProtectedDataDirFile();
        } else {
            dataDir = mPackageInfo.getDataDirFile();
        }
        if (dataDir == null) {
            throw new RuntimeException("No data directory found for package " + getPackageName());
        }
        return dataDir;
    }

    public File getFilesDir() {
        synchronized (mSync) {
            if (mFilesDir == null) {
                mFilesDir = new File(getDataDir(), "files");
            }
            return ensurePrivateDirExists(mFilesDir);
        }
    }

    public File getNoBackupFilesDir() {
        synchronized (mSync) {
            if (mNoBackupFilesDir == null) {
                mNoBackupFilesDir = new File(getDataDir(), "no_backup");
            }
            return ensurePrivateDirExists(mNoBackupFilesDir);
        }
    }

    public File getCacheDir() {
        synchronized (mSync) {
            if (mCacheDir == null) {
                mCacheDir = new File(getDataDir(), "cache");
            }
            return ensurePrivateCacheDirExists(mCacheDir, XATTR_INODE_CACHE);
        }
    }

    public File getCodeCacheDir() {
        synchronized (mSync) {
            if (mCodeCacheDir == null) {
                mCodeCacheDir = new File(getDataDir(), "code_cache");
            }
            return ensurePrivateCacheDirExists(mCodeCacheDir, XATTR_INODE_CODE_CACHE);
        }
    }

    public File getDir(String name, int mode) {
        checkMode(mode);
        File file = makeFilename(getDataDir(), "app_" + name);
        if (!file.exists()) {
            file.mkdir();
            setFilePermissionsFromMode(file.getPath(), mode,
                    FileUtils.S_IRWXU | FileUtils.S_IRWXG | FileUtils.S_IXOTH);
        }
        return file;
    }

    public File getDatabasePath(String name) {
        if (name.charAt(0) == File.separatorChar) {
            File dir = new File(name.substring(0, name.lastIndexOf(File.separatorChar)));
            File file = new File(dir, name.substring(name.lastIndexOf(File.separatorChar)));
            if (!dir.isDirectory() && dir.mkdir()) {
                FileUtils.setPermissions(dir.getPath(), FileUtils.S_IRWXU | FileUtils.S_IRWXG | FileUtils.S_IXOTH,
                        -1, -1);
            }
            return file;
        }
        return makeFilename(getDatabasesDir(), name);
    }

    public String[] databaseList() {
        return FileUtils.listOrEmpty(getDatabasesDir());
    }

    // The app's own databases (the compatibility plan's gap 7), as the tag's ContextImpl opens and deletes them.
    public SQLiteDatabase openOrCreateDatabase(String name, int mode, CursorFactory factory) {
        return openOrCreateDatabase(name, mode, factory, null);
    }

    public SQLiteDatabase openOrCreateDatabase(String name, int mode, CursorFactory factory,
            DatabaseErrorHandler errorHandler) {
        checkMode(mode);
        File f = getDatabasePath(name);
        int flags = SQLiteDatabase.CREATE_IF_NECESSARY;
        if ((mode & MODE_ENABLE_WRITE_AHEAD_LOGGING) != 0) {
            flags |= SQLiteDatabase.ENABLE_WRITE_AHEAD_LOGGING;
        }
        if ((mode & MODE_NO_LOCALIZED_COLLATORS) != 0) {
            flags |= SQLiteDatabase.NO_LOCALIZED_COLLATORS;
        }
        SQLiteDatabase db = SQLiteDatabase.openDatabase(f.getPath(), factory, flags, errorHandler);
        setFilePermissionsFromMode(f.getPath(), mode, 0);
        return db;
    }

    public boolean deleteDatabase(String name) {
        try {
            File f = getDatabasePath(name);
            return SQLiteDatabase.deleteDatabase(f);
        } catch (Exception e) {
        }
        return false;
    }

    public File getFileStreamPath(String name) {
        return makeFilename(getFilesDir(), name);
    }

    public String[] fileList() {
        return FileUtils.listOrEmpty(getFilesDir());
    }

    public FileInputStream openFileInput(String name) throws FileNotFoundException {
        return new FileInputStream(makeFilename(getFilesDir(), name));
    }

    public FileOutputStream openFileOutput(String name, int mode) throws FileNotFoundException {
        checkMode(mode);
        boolean append = (mode & MODE_APPEND) != 0;
        File file = makeFilename(getFilesDir(), name);
        try {
            FileOutputStream out = new FileOutputStream(file, append);
            setFilePermissionsFromMode(file.getPath(), mode, 0);
            return out;
        } catch (FileNotFoundException e) {
            // The files folder may be gone; make it again, as the tag does.
        }
        File parent = file.getParentFile();
        parent.mkdir();
        FileUtils.setPermissions(parent.getPath(), FileUtils.S_IRWXU | FileUtils.S_IRWXG | FileUtils.S_IXOTH, -1, -1);
        FileOutputStream out = new FileOutputStream(file, append);
        setFilePermissionsFromMode(file.getPath(), mode, 0);
        return out;
    }

    public boolean deleteFile(String name) {
        return makeFilename(getFilesDir(), name).delete();
    }

    public File getSharedPreferencesPath(String name) {
        return makeFilename(getPreferencesDir(), name + ".xml");
    }

    // SharedPreferences, as the tag's ContextImpl gives them: one SharedPreferencesImpl per file and package.

    public SharedPreferences getSharedPreferences(String name, int mode) {
        // Before KitKat a null name became "null.xml", and an app may still rely on it.
        if (mPackageInfo.getApplicationInfo().targetSdkVersion < Build.VERSION_CODES.KITKAT && name == null) {
            name = "null";
        }
        File file;
        synchronized (ContextImpl.class) {
            if (mSharedPrefsPaths == null) {
                mSharedPrefsPaths = new ArrayMap<>();
            }
            file = mSharedPrefsPaths.get(name);
            if (file == null) {
                file = getSharedPreferencesPath(name);
                mSharedPrefsPaths.put(name, file);
            }
        }
        return getSharedPreferences(file, mode);
    }

    /**
     * As the tag's: a context with the credential protected storage reads its preferences once the user is unlocked,
     * which Android's one user always is here (UserService).
     */
    public SharedPreferences getSharedPreferences(File file, int mode) {
        SharedPreferencesImpl sp;
        synchronized (ContextImpl.class) {
            ArrayMap<File, SharedPreferencesImpl> cache = getSharedPreferencesCacheLocked();
            sp = cache.get(file);
            if (sp == null) {
                checkMode(mode);
                if (getApplicationInfo().targetSdkVersion >= Build.VERSION_CODES.O) {
                    if (isCredentialProtectedStorage()) {
                        final UserManager um = getSystemService(UserManager.class);
                        if (um == null) {
                            throw new IllegalStateException("SharedPreferences cannot be accessed "
                                    + "if UserManager is not available. "
                                    + "(e.g. from inside an isolated process)");
                        }
                        if (!um.isUserUnlockingOrUnlocked(UserHandle.myUserId())) {
                            throw new IllegalStateException("SharedPreferences in "
                                    + "credential encrypted storage are not available until after "
                                    + "user (id " + UserHandle.myUserId() + ") is unlocked");
                        }
                    }
                }
                sp = new SharedPreferencesImpl(file, mode);
                cache.put(file, sp);
                return sp;
            }
        }
        if ((mode & MODE_MULTI_PROCESS) != 0
                || getApplicationInfo().targetSdkVersion < Build.VERSION_CODES.HONEYCOMB) {
            // As the tag: reloaded if the file changed behind the app's back.
            sp.startReloadIfChangedUnexpectedly();
        }
        return sp;
    }

    public void reloadSharedPreferences() {
        ArrayList<SharedPreferencesImpl> all = new ArrayList<>();
        synchronized (ContextImpl.class) {
            ArrayMap<File, SharedPreferencesImpl> cache = getSharedPreferencesCacheLocked();
            for (int i = 0; i < cache.size(); i++) {
                if (cache.valueAt(i) != null) {
                    all.add(cache.valueAt(i));
                }
            }
        }
        for (SharedPreferencesImpl sp : all) {
            sp.startReloadIfChangedUnexpectedly();
        }
    }

    public boolean moveSharedPreferencesFrom(Context sourceContext, String name) {
        synchronized (ContextImpl.class) {
            File source = sourceContext.getSharedPreferencesPath(name);
            File target = getSharedPreferencesPath(name);
            int moved = moveFiles(source.getParentFile(), target.getParentFile(), source.getName());
            if (moved > 0) {
                ArrayMap<File, SharedPreferencesImpl> cache = getSharedPreferencesCacheLocked();
                cache.remove(source);
                cache.remove(target);
            }
            return moved != -1;
        }
    }

    public boolean deleteSharedPreferences(String name) {
        synchronized (ContextImpl.class) {
            File prefs = getSharedPreferencesPath(name);
            File backup = SharedPreferencesImpl.makeBackupFile(prefs);
            getSharedPreferencesCacheLocked().remove(prefs);
            prefs.delete();
            backup.delete();
            return !(prefs.exists() || backup.exists());
        }
    }

    /** As the tag's: a move of the database's files, each copied, given the source's modes and then deleted. */
    public boolean moveDatabaseFrom(Context sourceContext, String name) {
        synchronized (ContextImpl.class) {
            File source = sourceContext.getDatabasePath(name);
            File target = getDatabasePath(name);
            return moveFiles(source.getParentFile(), target.getParentFile(), source.getName()) != -1;
        }
    }

    public boolean isCredentialProtectedStorage() {
        return (mFlags & Context.CONTEXT_CREDENTIAL_PROTECTED_STORAGE) != 0;
    }

    public boolean isDeviceProtectedStorage() {
        return (mFlags & Context.CONTEXT_DEVICE_PROTECTED_STORAGE) != 0;
    }

    // The contexts an app makes from its own, as the tag's ContextImpl makes them.

    /** As the tag's: the same package and token with the device protected storage. */
    public Context createDeviceProtectedStorageContext() {
        return (Context) (Object) copy((mFlags & ~Context.CONTEXT_CREDENTIAL_PROTECTED_STORAGE)
                | Context.CONTEXT_DEVICE_PROTECTED_STORAGE);
    }

    /** As the tag's: the same package and token with the credential protected storage. */
    public Context createCredentialProtectedStorageContext() {
        return (Context) (Object) copy((mFlags & ~Context.CONTEXT_DEVICE_PROTECTED_STORAGE)
                | Context.CONTEXT_CREDENTIAL_PROTECTED_STORAGE);
    }

    public Context createPackageContext(String packageName, int flags) throws PackageManager.NameNotFoundException {
        return createPackageContextAsUser(packageName, flags, Process.myUserHandle());
    }

    /**
     * As the tag's: "android" and "system" a copy of this context, whose Resources hold the system's too; the app's own
     * package a context with the package's Resources and this one's token; any other package is not installed in this
     * process.
     */
    public Context createPackageContextAsUser(String packageName, int flags, UserHandle user)
            throws PackageManager.NameNotFoundException {
        if (packageName.equals("system") || packageName.equals("android")) {
            return (Context) (Object) copy(flags);
        }
        if (packageName.equals(mPackageInfo.getPackageName())) {
            ContextImpl context = copy(flags);
            context.mResources = mPackageInfo.getResources();
            return (Context) (Object) context;
        }
        throw new PackageManager.NameNotFoundException("Application package " + packageName + " not found");
    }

    /** As the tag's for the app's own package: its context without a token; any other is not installed here. */
    public Context createApplicationContext(ApplicationInfo application, int flags)
            throws PackageManager.NameNotFoundException {
        if (application.packageName.equals(mPackageInfo.getPackageName())) {
            ContextImpl context = new ContextImpl(mMainThread, mPackageInfo, flags);
            context.mAttributionTag = mAttributionTag;
            return (Context) (Object) context;
        }
        throw new PackageManager.NameNotFoundException("Application package " + application.packageName
                + " not found");
    }

    /** As the tag's: the attribution tag this context was made for, null for the app's default. */
    public String getAttributionTag() {
        return mAttributionTag;
    }

    /** As the tag's: this context with the params' attribution tag. */
    public Context createContext(ContextParams contextParams) {
        ContextImpl context = copy(mFlags);
        context.mAttributionTag = contextParams.getAttributionTag();
        return (Context) (Object) context;
    }

    /** As the tag's, which goes through createContext: this context with another attribution tag. */
    public Context createAttributionContext(String attributionTag) {
        ContextImpl context = copy(mFlags);
        context.mAttributionTag = attributionTag;
        return (Context) (Object) context;
    }

    /**
     * As the tag's: Resources of their own with this context's configuration and the override on top, the rest as
     * this context.
     */
    public Context createConfigurationContext(Configuration overrideConfiguration) {
        if (overrideConfiguration == null) {
            throw new IllegalArgumentException("overrideConfiguration must not be null");
        }
        Configuration config = new Configuration(getResources().getConfiguration());
        config.updateFrom(overrideConfiguration);
        ContextImpl context = copy(mFlags);
        context.mResources = mPackageInfo.createActivityResources(config);
        return (Context) (Object) context;
    }

    /**
     * As the tag's: a context associated with the display, which is no UI context; Minato has display 0 only, so its
     * Resources are the package's.
     */
    public Context createDisplayContext(Display display) {
        if (display == null) {
            throw new IllegalArgumentException("display must not be null");
        }
        ContextImpl context = copy(mFlags);
        context.mIsActivityContext = false;
        context.mIsDisplayContext = true;
        context.mResources = mPackageInfo.getResources();
        return (Context) (Object) context;
    }

    /**
     * As the tag's: for an app that does not load its splits in isolation this context, as its class loader and
     * Resources hold every split; for one that does, a context with the split's class loader and Resources.
     */
    public Context createContextForSplit(String splitName) throws PackageManager.NameNotFoundException {
        if (!mPackageInfo.isolated()) {
            return (Context) (Object) this;
        }
        ClassLoader loader = mPackageInfo.getSplitClassLoader(splitName);
        Resources resources = mPackageInfo.getSplitResources(splitName);
        ContextImpl context = copy(mFlags);
        context.mClassLoader = loader;
        context.mResources = resources;
        return (Context) (Object) context;
    }

    // The external folders of decision 6, on Environment's one volume.

    public File getExternalFilesDir(String type) {
        File[] dirs = getExternalFilesDirs(type);
        return dirs != null && dirs.length > 0 ? dirs[0] : null;
    }

    public File[] getExternalFilesDirs(String type) {
        synchronized (mSync) {
            File[] dirs = Environment.buildExternalStorageAppFilesDirs(getPackageName());
            if (type != null) {
                dirs = Environment.buildPaths(dirs, type);
            }
            return ensureExternalDirsExistOrFilter(dirs);
        }
    }

    public File getExternalCacheDir() {
        File[] dirs = getExternalCacheDirs();
        return dirs != null && dirs.length > 0 ? dirs[0] : null;
    }

    public File[] getExternalCacheDirs() {
        synchronized (mSync) {
            return ensureExternalDirsExistOrFilter(Environment.buildExternalStorageAppCacheDirs(getPackageName()));
        }
    }

    public File getObbDir() {
        File[] dirs = getObbDirs();
        return dirs != null && dirs.length > 0 ? dirs[0] : null;
    }

    public File[] getObbDirs() {
        synchronized (mSync) {
            return ensureExternalDirsExistOrFilter(Environment.buildExternalStorageAppObbDirs(getPackageName()));
        }
    }

    public File[] getExternalMediaDirs() {
        synchronized (mSync) {
            return ensureExternalDirsExistOrFilter(Environment.buildExternalStorageAppMediaDirs(getPackageName()));
        }
    }

    // Permissions: the app's own uid holds what the manifest grants (ADR-0045 decision 6), root and system hold
    // everything, as ActivityManager's checkComponentPermission has it, and any other uid nothing, since the process
    // has no other package. Calls come from the app itself: there is no IPC to take another caller from.

    public int checkPermission(String permission, int pid, int uid) {
        if (permission == null) {
            throw new IllegalArgumentException("permission is null");
        }
        int appId = uid % 100000;
        if (appId == Process.ROOT_UID || appId == Process.SYSTEM_UID) {
            return PackageManager.PERMISSION_GRANTED;
        }
        if (uid != Process.myUid()) {
            return PackageManager.PERMISSION_DENIED;
        }
        ManifestPackage pkg = mMainThread.getManifestPackage();
        return pkg != null && pkg.granted(permission)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    public int checkPermission(String permission, int pid, int uid, IBinder callerToken) {
        return checkPermission(permission, pid, uid);
    }

    public int checkCallingPermission(String permission) {
        if (permission == null) {
            throw new IllegalArgumentException("permission is null");
        }
        // The tag's answer when the caller is this process, as every caller is here.
        return PackageManager.PERMISSION_DENIED;
    }

    public int checkCallingOrSelfPermission(String permission) {
        if (permission == null) {
            throw new IllegalArgumentException("permission is null");
        }
        return checkPermission(permission, Process.myPid(), Process.myUid());
    }

    public int checkSelfPermission(String permission) {
        if (permission == null) {
            throw new IllegalArgumentException("permission is null");
        }
        return checkPermission(permission, Process.myPid(), Process.myUid());
    }

    public void enforcePermission(String permission, int pid, int uid, String message) {
        enforce(permission, checkPermission(permission, pid, uid), false, uid, message);
    }

    public void enforceCallingPermission(String permission, String message) {
        enforce(permission, checkCallingPermission(permission), false, Process.myUid(), message);
    }

    public void enforceCallingOrSelfPermission(String permission, String message) {
        enforce(permission, checkCallingOrSelfPermission(permission), true, Process.myUid(), message);
    }

    // URI permissions: no grants exist without system_server, so only root and system hold one. Android also lets
    // the app reach its own providers' URIs; that comes with the providers in step c2.

    public int checkUriPermission(Uri uri, int pid, int uid, int modeFlags) {
        return uriPermission(uid);
    }

    public int checkUriPermission(Uri uri, int pid, int uid, int modeFlags, IBinder callerToken) {
        return uriPermission(uid);
    }

    public int[] checkUriPermissions(List<Uri> uris, int pid, int uid, int modeFlags) {
        return uriPermissions(uris.size(), uid);
    }

    public int checkCallingUriPermission(Uri uri, int modeFlags) {
        return PackageManager.PERMISSION_DENIED;
    }

    public int[] checkCallingUriPermissions(List<Uri> uris, int modeFlags) {
        return uriPermissions(uris.size(), -1);
    }

    public int checkCallingOrSelfUriPermission(Uri uri, int modeFlags) {
        return uriPermission(Process.myUid());
    }

    public int[] checkCallingOrSelfUriPermissions(List<Uri> uris, int modeFlags) {
        return uriPermissions(uris.size(), Process.myUid());
    }

    public int checkUriPermission(Uri uri, String readPermission, String writePermission, int pid, int uid,
            int modeFlags) {
        return uriPermission(uid);
    }

    public void enforceUriPermission(Uri uri, int pid, int uid, int modeFlags, String message) {
        enforceUri(uriPermission(uid), modeFlags, uid, uri, message);
    }

    public void enforceCallingUriPermission(Uri uri, int modeFlags, String message) {
        enforceUri(PackageManager.PERMISSION_DENIED, modeFlags, Process.myUid(), uri, message);
    }

    public void enforceCallingOrSelfUriPermission(Uri uri, int modeFlags, String message) {
        enforceUri(uriPermission(Process.myUid()), modeFlags, Process.myUid(), uri, message);
    }

    public void enforceUriPermission(Uri uri, String readPermission, String writePermission, int pid, int uid,
            int modeFlags, String message) {
        enforceUri(uriPermission(uid), modeFlags, uid, uri, message);
    }

    // Starts from a context that is not an activity's, as the tag's ContextImpl makes them (ADR-0039 step b3.5b2-1):
    // Instrumentation's, which LocalActivityTaskManager answers.

    public void startActivity(Intent intent) {
        startActivity(intent, null);
    }

    /**
     * As the tag's: outside an activity a start needs FLAG_ACTIVITY_NEW_TASK, for a target SDK below 24 or from 28 on,
     * unless its options name the task to launch in, which ActivityOptions.fromBundle reads from this key, as
     * framework.jar has no ActivityOptions yet.
     */
    public void startActivity(Intent intent, Bundle options) {
        final int targetSdkVersion = getApplicationInfo().targetSdkVersion;
        if ((intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) == 0
                && (targetSdkVersion < Build.VERSION_CODES.N || targetSdkVersion >= Build.VERSION_CODES.P)
                && (options == null || options.getInt("android.activity.launchTaskId", -1) == -1)) {
            throw new AndroidRuntimeException("Calling startActivity() from outside of an Activity"
                    + " context requires the FLAG_ACTIVITY_NEW_TASK flag. Is this really what you want?");
        }
        mMainThread.getInstrumentation().execStartActivity(getOuterContext(), mMainThread.getApplicationThread(),
                null, (Activity) null, intent, -1, options);
    }

    public void startActivities(Intent[] intents) {
        startActivities(intents, null);
    }

    /** As the tag's: outside an activity the first intent needs FLAG_ACTIVITY_NEW_TASK. */
    public void startActivities(Intent[] intents, Bundle options) {
        if ((intents[0].getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) == 0) {
            throw new AndroidRuntimeException("Calling startActivities() from outside of an Activity"
                    + " context requires the FLAG_ACTIVITY_NEW_TASK flag on first Intent. Is this really what you"
                    + " want?");
        }
        mMainThread.getInstrumentation().execStartActivities(getOuterContext(), mMainThread.getApplicationThread(),
                null, (Activity) null, intents, options);
    }

    // Helpers.

    private static int uriPermission(int uid) {
        int appId = uid % 100000;
        return uid >= 0 && (appId == Process.ROOT_UID || appId == Process.SYSTEM_UID)
                ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
    }

    private static int[] uriPermissions(int count, int uid) {
        int[] result = new int[count];
        java.util.Arrays.fill(result, uriPermission(uid));
        return result;
    }

    private static void enforce(String permission, int result, boolean selfToo, int uid, String message) {
        if (result != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException((message != null ? message + ": " : "")
                    + (selfToo ? "Neither user " + uid + " nor current process has " : "uid " + uid
                    + " does not have ") + permission + ".");
        }
    }

    private static void enforceUri(int result, int modeFlags, int uid, Uri uri, String message) {
        if (result != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException((message != null ? message + ": " : "") + "User " + uid
                    + " does not have " + uriModeFlagToString(modeFlags) + " permission on " + uri + ".");
        }
    }

    private static String uriModeFlagToString(int uriModeFlags) {
        StringBuilder builder = new StringBuilder();
        if ((uriModeFlags & android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
            builder.append("read and ");
        }
        if ((uriModeFlags & android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION) != 0) {
            builder.append("write and ");
        }
        if ((uriModeFlags & android.content.Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION) != 0) {
            builder.append("persistable and ");
        }
        if ((uriModeFlags & android.content.Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) != 0) {
            builder.append("prefix and ");
        }
        if (builder.length() > 5) {
            builder.setLength(builder.length() - 5);
            return builder.toString();
        }
        throw new IllegalArgumentException("Unknown permission mode flags: " + uriModeFlags);
    }

    private ArrayMap<File, SharedPreferencesImpl> getSharedPreferencesCacheLocked() {
        if (sSharedPrefsCache == null) {
            sSharedPrefsCache = new ArrayMap<>();
        }
        ArrayMap<File, SharedPreferencesImpl> packagePrefs = sSharedPrefsCache.get(getPackageName());
        if (packagePrefs == null) {
            packagePrefs = new ArrayMap<>();
            sSharedPrefsCache.put(getPackageName(), packagePrefs);
        }
        return packagePrefs;
    }

    /** The tag's: moves the files of `sourceDir` that start with `prefix`; how many, or -1 after a failure. */
    private static int moveFiles(File sourceDir, File targetDir, String prefix) {
        int moved = 0;
        for (File source : FileUtils.listFilesOrEmpty(sourceDir, (dir, name) -> name.startsWith(prefix))) {
            File target = new File(targetDir, source.getName());
            Log.d(TAG, "Migrating " + source + " to " + target);
            try {
                FileUtils.copyFileOrThrow(source, target);
                FileUtils.copyPermissions(source, target);
                if (!source.delete()) {
                    throw new IOException("Failed to clean up " + source);
                }
                if (moved != -1) {
                    moved++;
                }
            } catch (IOException e) {
                Log.w(TAG, "Failed to migrate " + source + ": " + e);
                moved = -1;
            }
        }
        return moved;
    }

    private File getDatabasesDir() {
        synchronized (mSync) {
            if (mDatabasesDir == null) {
                mDatabasesDir = new File(getDataDir(), "databases");
            }
            return ensurePrivateDirExists(mDatabasesDir);
        }
    }

    private File getPreferencesDir() {
        synchronized (mSync) {
            if (mPreferencesDir == null) {
                mPreferencesDir = new File(getDataDir(), "shared_prefs");
            }
            return ensurePrivateDirExists(mPreferencesDir);
        }
    }

    /** The tag's ensurePrivateDirExists for an app's own folder. */
    private static File ensurePrivateDirExists(File file) {
        return ensurePrivateDirExists(file, 0771, -1, null);
    }

    /** For a cache folder: setgid, the app's cache gid, and its inode in the parent's extended attribute. */
    private static File ensurePrivateCacheDirExists(File file, String xattr) {
        int gid = UserHandle.getCacheAppGid(Process.myUid());
        return ensurePrivateDirExists(file, 02771, gid, xattr);
    }

    private static File ensurePrivateDirExists(File file, int mode, int gid, String xattr) {
        if (!file.exists()) {
            String path = file.getAbsolutePath();
            try {
                Os.mkdir(path, mode);
                Os.chmod(path, mode);
                if (gid != -1) {
                    Os.chown(path, -1, gid);
                }
            } catch (ErrnoException e) {
                if (e.errno != OsConstants.EEXIST) {
                    Log.w(TAG, "Failed to ensure " + file + ": " + e.getMessage());
                }
            }
            if (xattr != null) {
                try {
                    byte[] value = new byte[8];
                    Memory.pokeLong(value, 0, Os.stat(path).st_ino, ByteOrder.nativeOrder());
                    Os.setxattr(file.getParentFile().getAbsolutePath(), xattr, value, 0);
                } catch (ErrnoException e) {
                    Log.w(TAG, "Failed to update " + xattr + ": " + e.getMessage());
                }
            }
        }
        return file;
    }

    /**
     * The tag's ensureExternalDirsExistOrFilter without StorageManager: a folder the app cannot make becomes null.
     * Android asks the storage service for what the app cannot make, also for every cache folder; Minato's app owns
     * its volume's folders, so it makes them all itself.
     */
    private static File[] ensureExternalDirsExistOrFilter(File[] dirs) {
        File[] result = new File[dirs.length];
        for (int i = 0; i < dirs.length; i++) {
            File dir = dirs[i];
            if (!dir.exists() && !dir.mkdirs() && !dir.exists()) {
                Log.w(TAG, "Failed to ensure " + dir);
                dir = null;
            }
            result[i] = dir;
        }
        return result;
    }

    private void checkMode(int mode) {
        if (getApplicationInfo().targetSdkVersion >= Build.VERSION_CODES.N) {
            if ((mode & MODE_WORLD_READABLE) != 0) {
                throw new SecurityException("MODE_WORLD_READABLE no longer supported");
            }
            if ((mode & MODE_WORLD_WRITEABLE) != 0) {
                throw new SecurityException("MODE_WORLD_WRITEABLE no longer supported");
            }
        }
    }

    /**
     * The tag's: owner and group may read and write, and the mode's world bits. SharedPreferencesImpl calls it for
     * its file.
     *
     * @hide
     */
    @SuppressWarnings("deprecation")
    static void setFilePermissionsFromMode(String name, int mode, int extraPermissions) {
        int perms = FileUtils.S_IRUSR | FileUtils.S_IWUSR | FileUtils.S_IRGRP | FileUtils.S_IWGRP | extraPermissions;
        if ((mode & MODE_WORLD_READABLE) != 0) {
            perms |= FileUtils.S_IROTH;
        }
        if ((mode & MODE_WORLD_WRITEABLE) != 0) {
            perms |= FileUtils.S_IWOTH;
        }
        FileUtils.setPermissions(name, perms, -1, -1);
    }

    private static File makeFilename(File base, String name) {
        if (name.indexOf(File.separatorChar) < 0) {
            File file = new File(base, name);
            // As the tag: reported as file system access, for apps that hand the path to native code.
            BlockGuard.getVmPolicy().onPathAccess(file.getPath());
            return file;
        }
        throw new IllegalArgumentException("File " + name + " contains a path separator");
    }

    /**
     * The tag's ApplicationContentResolver: providers through ActivityThread.acquireProvider, by their authority
     * without a user, for the user it names or the context's. They are the process's own, so a stable and an
     * unstable reference are the same, and none dies apart from the process.
     */
    private static final class ApplicationContentResolver extends ContentResolver {
        private final ActivityThread mMainThread;

        ApplicationContentResolver(Context context, ActivityThread mainThread) {
            super(context);
            mMainThread = mainThread;
        }

        @Override
        protected IContentProvider acquireProvider(Context context, String auth) {
            return mMainThread.acquireProvider(context, ContentProvider.getAuthorityWithoutUserId(auth),
                    resolveUserIdFromAuthority(auth), true);
        }

        @Override
        public boolean releaseProvider(IContentProvider provider) {
            return mMainThread.releaseProvider(provider, true);
        }

        @Override
        protected IContentProvider acquireUnstableProvider(Context context, String auth) {
            return mMainThread.acquireProvider(context, ContentProvider.getAuthorityWithoutUserId(auth),
                    resolveUserIdFromAuthority(auth), false);
        }

        @Override
        public boolean releaseUnstableProvider(IContentProvider provider) {
            return mMainThread.releaseProvider(provider, false);
        }

        @Override
        public void unstableProviderDied(IContentProvider provider) {
        }

        private int resolveUserIdFromAuthority(String auth) {
            return ContentProvider.getUserIdFromAuthority(auth, getUserId());
        }
    }
}
