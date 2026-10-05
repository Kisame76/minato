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
import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.Context;
import android.content.IContentProvider;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.hardware.SensorManager;
import android.hardware.SystemSensorManager;
import android.hardware.input.InputManager;
import android.net.wifi.WifiManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.FileUtils;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.UserHandle;
import android.os.storage.StorageManager;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
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
    private LayoutInflater mLayoutInflater;
    private WindowManager mWindowManager;
    private SensorManager mSensorManager;
    private WifiManager mWifiManager;
    private InputManager mInputManager;
    private StorageManager mStorageManager;
    private AppOpsManager mAppOpsManager;
    /** An activity context's token and its own Resources; the application's has neither. */
    private IBinder mToken;
    private boolean mIsActivityContext;
    private Resources mResources;

    private ContextImpl(ActivityThread mainThread, LoadedApk packageInfo) {
        mMainThread = mainThread;
        mPackageInfo = packageInfo;
        mOpPackageName = packageInfo.getPackageName();
        mContentResolver = new ApplicationContentResolver((Context) (Object) this, mainThread);
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
        context.mResources = packageInfo.createActivityResources(overrideConfiguration);
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
     * The LayoutInflater and the sensor, Wi-Fi and input managers on the outer context, and the storage and app ops
     * managers and the window manager on this one, each made once per context as SystemServiceRegistry's
     * CachedServiceFetcher keeps them; every other service is still unwritten, logged once per name, so that the log
     * says which one an app asked for.
     */
    public Object getSystemService(String name) {
        if (Context.LAYOUT_INFLATER_SERVICE.equals(name)) {
            synchronized (mSync) {
                if (mLayoutInflater == null) {
                    mLayoutInflater = new PhoneLayoutInflater(getOuterContext());
                }
                return mLayoutInflater;
            }
        }
        if (Context.WINDOW_SERVICE.equals(name)) {
            synchronized (mSync) {
                if (mWindowManager == null) {
                    mWindowManager = new WindowManagerImpl((Context) (Object) this);
                }
                return mWindowManager;
            }
        }
        if (Context.SENSOR_SERVICE.equals(name)) {
            synchronized (mSync) {
                if (mSensorManager == null) {
                    mSensorManager = new SystemSensorManager(getOuterContext(), Looper.getMainLooper());
                }
                return mSensorManager;
            }
        }
        if (Context.WIFI_SERVICE.equals(name)) {
            synchronized (mSync) {
                if (mWifiManager == null) {
                    mWifiManager = new WifiManager(getOuterContext());
                }
                return mWifiManager;
            }
        }
        if (Context.INPUT_SERVICE.equals(name)) {
            synchronized (mSync) {
                if (mInputManager == null) {
                    mInputManager = new InputManager(getOuterContext());
                }
                return mInputManager;
            }
        }
        if (Context.STORAGE_SERVICE.equals(name)) {
            synchronized (mSync) {
                if (mStorageManager == null) {
                    mStorageManager = new StorageManager((Context) (Object) this, Looper.getMainLooper());
                }
                return mStorageManager;
            }
        }
        if (Context.APP_OPS_SERVICE.equals(name)) {
            synchronized (mSync) {
                if (mAppOpsManager == null) {
                    mAppOpsManager = new AppOpsManager((Context) (Object) this);
                }
                return mAppOpsManager;
            }
        }
        if (Context.INPUT_METHOD_SERVICE.equals(name)) {
            // The process's manager, without an input method (ADR-0043 step d)), as SystemServiceRegistry's forContext.
            return InputMethodManager.forContext(getOuterContext());
        }
        Unwritten.log("android.app.ContextImpl.getSystemService(String) for \"" + name + "\"", "null");
        return null;
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

    /** As the tag's: only a context associated with a display, an activity's here, has one to give. */
    public Display getDisplay() {
        if (!mIsActivityContext) {
            throw new UnsupportedOperationException("Tried to obtain display from a Context not associated with one."
                    + " Only visual Contexts (such as Activity or one created with Context#createWindowContext) or"
                    + " ones created with Context#createDisplayContext are associated with displays. Other types of"
                    + " Contexts are typically related to background entities and may return an arbitrary display.");
        }
        return getDisplayNoVerify();
    }

    /** @hide */
    public int getAssociatedDisplayId() {
        return mIsActivityContext ? getDisplayId() : Display.INVALID_DISPLAY;
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
        if (serviceClass == LayoutInflater.class) {
            return Context.LAYOUT_INFLATER_SERVICE;
        }
        if (serviceClass == WindowManager.class) {
            return Context.WINDOW_SERVICE;
        }
        if (serviceClass == InputMethodManager.class) {
            return Context.INPUT_METHOD_SERVICE;
        }
        if (serviceClass == SensorManager.class) {
            return Context.SENSOR_SERVICE;
        }
        if (serviceClass == WifiManager.class) {
            return Context.WIFI_SERVICE;
        }
        if (serviceClass == InputManager.class) {
            return Context.INPUT_SERVICE;
        }
        if (serviceClass == StorageManager.class) {
            return Context.STORAGE_SERVICE;
        }
        if (serviceClass == AppOpsManager.class) {
            return Context.APP_OPS_SERVICE;
        }
        Unwritten.log("android.app.ContextImpl.getSystemServiceName(Class) for "
                + (serviceClass == null ? "null" : serviceClass.getName()), "null");
        return null;
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

    /** The Application, once LoadedApk made it: null while its attachBaseContext runs, as on Android. */
    public Context getApplicationContext() {
        return mPackageInfo.getApplication();
    }

    public ClassLoader getClassLoader() {
        return mPackageInfo.getClassLoader();
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

    public File getDataDir() {
        File dataDir = mPackageInfo.getDataDirFile();
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

    /** The tag's, without its check of credential-protected storage, which an app's context is not. */
    public SharedPreferences getSharedPreferences(File file, int mode) {
        SharedPreferencesImpl sp;
        synchronized (ContextImpl.class) {
            ArrayMap<File, SharedPreferencesImpl> cache = getSharedPreferencesCacheLocked();
            sp = cache.get(file);
            if (sp == null) {
                checkMode(mode);
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

    /** An app's context is neither of the two storage contexts. */
    public boolean isCredentialProtectedStorage() {
        return false;
    }

    public boolean isDeviceProtectedStorage() {
        return false;
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
