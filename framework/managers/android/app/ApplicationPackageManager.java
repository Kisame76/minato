// The hand file of our ApplicationPackageManager (ADR-0039 decision 7, b3.5a): tools/managergen --implement writes
// the class from PackageManager's abstract methods, and the members here replace the generated ones. It answers from
// the app's manifest as ActivityThread read it, without IPackageManager.
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.util.Log;

class ApplicationPackageManager {
    private final ActivityThread mThread;

    /**
     * Ours: Android's takes the ContextImpl and IPackageManager. Step c's ContextImpl calls this one.
     *
     * @hide
     */
    public ApplicationPackageManager(ActivityThread thread) {
        mThread = thread;
    }

    /**
     * ResourcesManager calls it at a configuration change. Android's clears its icon and label caches; ours keeps
     * none.
     *
     * @hide
     */
    static void configurationChanged() {
    }

    // The app's own package (b3.5a), as Android's answers through IPackageManager; any other package is unknown, as
    // decision 7 has it, Play services and the Play Store among them. The int overloads' flags are the long ones'.

    private ManifestPackage own(String packageName) throws NameNotFoundException {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || !pkg.packageName.equals(packageName)) {
            throw new NameNotFoundException(packageName);
        }
        return pkg;
    }

    public PackageInfo getPackageInfo(String packageName, int flags) throws NameNotFoundException {
        return getPackageInfo(packageName, PackageInfoFlags.of(flags));
    }

    public PackageInfo getPackageInfo(String packageName, PackageInfoFlags flags) throws NameNotFoundException {
        return own(packageName).packageInfo(flags.getValue());
    }

    public ApplicationInfo getApplicationInfo(String packageName, int flags) throws NameNotFoundException {
        return getApplicationInfo(packageName, ApplicationInfoFlags.of(flags));
    }

    public ApplicationInfo getApplicationInfo(String packageName, ApplicationInfoFlags flags)
            throws NameNotFoundException {
        return own(packageName).applicationInfo(flags.getValue());
    }

    public ActivityInfo getActivityInfo(ComponentName className, int flags) throws NameNotFoundException {
        return getActivityInfo(className, ComponentInfoFlags.of(flags));
    }

    public ActivityInfo getActivityInfo(ComponentName className, ComponentInfoFlags flags)
            throws NameNotFoundException {
        ActivityInfo info = component(className).activityInfo(className, flags.getValue(), false);
        if (info == null) {
            throw new NameNotFoundException(className.toString());
        }
        return info;
    }

    public ActivityInfo getReceiverInfo(ComponentName className, int flags) throws NameNotFoundException {
        return getReceiverInfo(className, ComponentInfoFlags.of(flags));
    }

    public ActivityInfo getReceiverInfo(ComponentName className, ComponentInfoFlags flags)
            throws NameNotFoundException {
        ActivityInfo info = component(className).activityInfo(className, flags.getValue(), true);
        if (info == null) {
            throw new NameNotFoundException(className.toString());
        }
        return info;
    }

    public ServiceInfo getServiceInfo(ComponentName className, int flags) throws NameNotFoundException {
        return getServiceInfo(className, ComponentInfoFlags.of(flags));
    }

    public ServiceInfo getServiceInfo(ComponentName className, ComponentInfoFlags flags)
            throws NameNotFoundException {
        ServiceInfo info = component(className).serviceInfo(className, flags.getValue());
        if (info == null) {
            throw new NameNotFoundException(className.toString());
        }
        return info;
    }

    public ProviderInfo getProviderInfo(ComponentName className, int flags) throws NameNotFoundException {
        return getProviderInfo(className, ComponentInfoFlags.of(flags));
    }

    public ProviderInfo getProviderInfo(ComponentName className, ComponentInfoFlags flags)
            throws NameNotFoundException {
        ProviderInfo info = component(className).providerInfo(className, flags.getValue());
        if (info == null) {
            throw new NameNotFoundException(className.toString());
        }
        return info;
    }

    /** A component's package, which must be the app's; Android names the component when it is unknown. */
    private ManifestPackage component(ComponentName className) throws NameNotFoundException {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || !pkg.packageName.equals(className.getPackageName())) {
            throw new NameNotFoundException(className.toString());
        }
        return pkg;
    }

    public CharSequence getApplicationLabel(ApplicationInfo info) {
        return info.loadLabel(this);
    }

    /** The app's own string, from the Resources the manifest was read with; null for any other package's. */
    public CharSequence getText(String packageName, int resid, ApplicationInfo appInfo) {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || !pkg.packageName.equals(packageName)) {
            return null;
        }
        try {
            return pkg.resources.getText(resid);
        } catch (RuntimeException e) {
            Log.w("PackageManager", "Failure retrieving text 0x" + Integer.toHexString(resid) + " in package "
                    + packageName, e);
            return null;
        }
    }

    /**
     * Android's parses the archive with AOSP's package parser, which decision 7 rejects; so PackageManager's anonymous
     * PackageParser2.Callback, which stands on a stub (framework/stub-supertypes.txt), never loads. Ours answers null,
     * Android's answer for an archive it cannot parse, until an app needs more.
     */
    public PackageInfo getPackageArchiveInfo(String archiveFilePath, PackageInfoFlags flags) {
        Unwritten.log("android.app.ApplicationPackageManager.getPackageArchiveInfo(String,PackageInfoFlags)", "null");
        return null;
    }
}
