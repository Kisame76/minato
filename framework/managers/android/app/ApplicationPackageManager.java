// The hand file of our ApplicationPackageManager (ADR-0039 decision 7, b3.5a and b3.5b1): tools/managergen --implement
// writes the class from PackageManager's abstract methods, and the members here replace the generated ones. It
// answers from the app's manifest as ActivityThread read it, the platform's permissions and the import's signers,
// without IPackageManager.
import android.app.Application;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.LocalPackageInstaller;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageItemInfo;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.SigningDetails;
import android.content.pm.VersionedPackage;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.os.SystemProperties;
import android.os.UserHandle;
import android.util.ArrayMap;
import android.util.Log;
import com.android.minato.pm.SystemFeatures;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;

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
        if (ANDROID.equals(packageName)) {
            return androidPackageInfo(flags.getValue());
        }
        return own(packageName).packageInfo(flags.getValue());
    }

    public ApplicationInfo getApplicationInfo(String packageName, int flags) throws NameNotFoundException {
        return getApplicationInfo(packageName, ApplicationInfoFlags.of(flags));
    }

    public ApplicationInfo getApplicationInfo(String packageName, ApplicationInfoFlags flags)
            throws NameNotFoundException {
        if (ANDROID.equals(packageName)) {
            return androidApplicationInfo();
        }
        return own(packageName).applicationInfo(flags.getValue());
    }

    // The app's identity (the package manager stub plan, P2) [verified: package-answers-test on apk-runner-35]: its
    // uid's packages and name, its uid and gids, its PackageInfo by version (PackageManagerService checks the version
    // only for a static shared library, so any one finds the package), its enabled setting, and "android", the
    // platform's package, which every app sees: framework-res as the system's uid 1000. The installed packages an app
    // sees are its own and "android"; a device's other force-queryable system packages are not there.

    private static final String ANDROID = "android";

    private static ApplicationInfo androidApplicationInfo() {
        ApplicationInfo info = new ApplicationInfo();
        info.packageName = ANDROID;
        info.processName = "system";
        info.uid = Process.SYSTEM_UID;
        info.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_PERSISTENT | ApplicationInfo.FLAG_INSTALLED;
        info.enabled = true;
        info.sourceDir = "/system/framework/framework-res.apk";
        info.publicSourceDir = info.sourceDir;
        info.targetSdkVersion = Build.VERSION.SDK_INT;
        info.minSdkVersion = Build.VERSION.SDK_INT;
        info.nonLocalizedLabel = "Android System";
        return info;
    }

    private static PackageInfo androidPackageInfo(long flags) {
        PackageInfo info = new PackageInfo();
        info.packageName = ANDROID;
        info.sharedUserId = "android.uid.system";
        info.setLongVersionCode(Build.VERSION.SDK_INT);
        info.versionName = Build.VERSION.RELEASE;
        info.applicationInfo = androidApplicationInfo();
        return info;
    }

    public PackageInfo getPackageInfo(VersionedPackage versionedPackage, int flags) throws NameNotFoundException {
        return getPackageInfo(versionedPackage, PackageInfoFlags.of(flags));
    }

    public PackageInfo getPackageInfo(VersionedPackage versionedPackage, PackageInfoFlags flags)
            throws NameNotFoundException {
        try {
            return getPackageInfo(versionedPackage.getPackageName(), flags);
        } catch (NameNotFoundException e) {
            throw new NameNotFoundException(versionedPackage.toString());
        }
    }

    public PackageInfo getPackageInfoAsUser(String packageName, int flags, int userId) throws NameNotFoundException {
        return getPackageInfo(packageName, PackageInfoFlags.of(flags));
    }

    public PackageInfo getPackageInfoAsUser(String packageName, PackageInfoFlags flags, int userId)
            throws NameNotFoundException {
        return getPackageInfo(packageName, flags);
    }

    public ApplicationInfo getApplicationInfoAsUser(String packageName, int flags, int userId)
            throws NameNotFoundException {
        return getApplicationInfo(packageName, ApplicationInfoFlags.of(flags));
    }

    public ApplicationInfo getApplicationInfoAsUser(String packageName, ApplicationInfoFlags flags, int userId)
            throws NameNotFoundException {
        return getApplicationInfo(packageName, flags);
    }

    public List<PackageInfo> getInstalledPackages(int flags) {
        return getInstalledPackages(PackageInfoFlags.of(flags));
    }

    public List<PackageInfo> getInstalledPackages(PackageInfoFlags flags) {
        List<PackageInfo> packages = new ArrayList<>();
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg != null) {
            packages.add(pkg.packageInfo(flags.getValue()));
        }
        packages.add(androidPackageInfo(flags.getValue()));
        return packages;
    }

    public List<PackageInfo> getInstalledPackagesAsUser(int flags, int userId) {
        return getInstalledPackages(PackageInfoFlags.of(flags));
    }

    public List<PackageInfo> getInstalledPackagesAsUser(PackageInfoFlags flags, int userId) {
        return getInstalledPackages(flags);
    }

    public List<ApplicationInfo> getInstalledApplications(int flags) {
        return getInstalledApplications(ApplicationInfoFlags.of(flags));
    }

    public List<ApplicationInfo> getInstalledApplications(ApplicationInfoFlags flags) {
        List<ApplicationInfo> applications = new ArrayList<>();
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg != null) {
            applications.add(pkg.applicationInfo(flags.getValue()));
        }
        applications.add(androidApplicationInfo());
        return applications;
    }

    public List<ApplicationInfo> getInstalledApplicationsAsUser(int flags, int userId) {
        return getInstalledApplications(ApplicationInfoFlags.of(flags));
    }

    public List<ApplicationInfo> getInstalledApplicationsAsUser(ApplicationInfoFlags flags, int userId) {
        return getInstalledApplications(flags);
    }

    /** The app's uid has its package; any other uid has none the app sees. */
    public String[] getPackagesForUid(int uid) {
        ManifestPackage pkg = mThread.getManifestPackage();
        return pkg != null && uid == Process.myUid() ? new String[] {pkg.packageName} : null;
    }

    /** As PackageManagerService names a uid: its shared user's name and the uid, else its package. */
    public String getNameForUid(int uid) {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || uid != Process.myUid()) {
            return null;
        }
        String sharedUserId = pkg.packageInfo(0).sharedUserId;
        return sharedUserId != null ? sharedUserId + ":" + uid : pkg.packageName;
    }

    public String[] getNamesForUids(int[] uids) {
        String[] names = new String[uids.length];
        for (int i = 0; i < uids.length; i++) {
            names[i] = getNameForUid(uids[i]);
        }
        return names;
    }

    public int getPackageUid(String packageName, int flags) throws NameNotFoundException {
        return getPackageUid(packageName, PackageInfoFlags.of(flags));
    }

    public int getPackageUid(String packageName, PackageInfoFlags flags) throws NameNotFoundException {
        return getApplicationInfo(packageName, ApplicationInfoFlags.of(flags.getValue())).uid;
    }

    public int getPackageUidAsUser(String packageName, int userId) throws NameNotFoundException {
        return getPackageUid(packageName, 0);
    }

    public int getPackageUidAsUser(String packageName, int flags, int userId) throws NameNotFoundException {
        return getPackageUid(packageName, flags);
    }

    public int getPackageUidAsUser(String packageName, PackageInfoFlags flags, int userId)
            throws NameNotFoundException {
        return getPackageUid(packageName, flags);
    }

    /** The platform's gids of the normal permissions (platform.xml at the tag); the others no app here holds. */
    private static final String[] GID_PERMISSIONS = {
        "android.permission.BLUETOOTH_ADMIN", "android.permission.BLUETOOTH", "android.permission.INTERNET",
    };
    /** AID_NET_BT_ADMIN, AID_NET_BT and AID_INET. */
    private static final int[] GIDS = {3001, 3002, 3003};

    public int[] getPackageGids(String packageName) throws NameNotFoundException {
        return getPackageGids(packageName, 0);
    }

    public int[] getPackageGids(String packageName, int flags) throws NameNotFoundException {
        return getPackageGids(packageName, PackageInfoFlags.of(flags));
    }

    /** The gids of the package's granted permissions that have one, as PermissionManagerService gives them. */
    public int[] getPackageGids(String packageName, PackageInfoFlags flags) throws NameNotFoundException {
        ManifestPackage pkg = own(packageName);
        int count = 0;
        int[] gids = new int[GIDS.length];
        for (int i = 0; i < GID_PERMISSIONS.length; i++) {
            if (pkg.granted(GID_PERMISSIONS[i])) {
                gids[count++] = GIDS[i];
            }
        }
        return java.util.Arrays.copyOf(gids, count);
    }

    /** The app's package is in its default state, as nothing here changes it; any other package is unknown. */
    public int getApplicationEnabledSetting(String packageName) {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || !pkg.packageName.equals(packageName)) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    /**
     * As AOSP's for a property that is not defined, in any package (ADR-0045's storage and app ops step:
     * Environment's isExternalStorageLegacy asks PROPERTY_NO_APP_DATA_STORAGE): the package reader keeps no property
     * elements yet, a listed gap, so every one is undefined.
     */
    public Property getProperty(String propertyName, String packageName) throws NameNotFoundException {
        Objects.requireNonNull(packageName);
        Objects.requireNonNull(propertyName);
        throw new NameNotFoundException();
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

    // Intent resolution over the app's own components (ADR-0045 step SV1), as Android's answers through
    // IPackageManager for one installed package: ManifestPackage.query has the rules. The type is the intent's own or
    // its content: URI's, as ApplicationPackageManager resolves it with the context's ContentResolver.

    public List<ResolveInfo> queryIntentActivities(Intent intent, int flags) {
        return queryIntentActivities(intent, ResolveInfoFlags.of(flags));
    }

    public List<ResolveInfo> queryIntentActivities(Intent intent, ResolveInfoFlags flags) {
        return query(ManifestPackage.QUERY_ACTIVITIES, intent, flags.getValue());
    }

    public List<ResolveInfo> queryBroadcastReceivers(Intent intent, int flags) {
        return queryBroadcastReceivers(intent, ResolveInfoFlags.of(flags));
    }

    public List<ResolveInfo> queryBroadcastReceivers(Intent intent, ResolveInfoFlags flags) {
        return query(ManifestPackage.QUERY_RECEIVERS, intent, flags.getValue());
    }

    public List<ResolveInfo> queryIntentServices(Intent intent, int flags) {
        return queryIntentServices(intent, ResolveInfoFlags.of(flags));
    }

    public List<ResolveInfo> queryIntentServices(Intent intent, ResolveInfoFlags flags) {
        return query(ManifestPackage.QUERY_SERVICES, intent, flags.getValue());
    }

    public List<ResolveInfo> queryIntentContentProviders(Intent intent, int flags) {
        return queryIntentContentProviders(intent, ResolveInfoFlags.of(flags));
    }

    public List<ResolveInfo> queryIntentContentProviders(Intent intent, ResolveInfoFlags flags) {
        return query(ManifestPackage.QUERY_PROVIDERS, intent, flags.getValue());
    }

    // The hidden forms for a user (P2), which AOSP's own framework code calls: the one user's answers.

    public ResolveInfo resolveActivityAsUser(Intent intent, int flags, int userId) {
        return resolveActivity(intent, flags);
    }

    public ResolveInfo resolveActivityAsUser(Intent intent, ResolveInfoFlags flags, int userId) {
        return resolveActivity(intent, flags);
    }

    public List<ResolveInfo> queryIntentActivitiesAsUser(Intent intent, int flags, int userId) {
        return queryIntentActivities(intent, flags);
    }

    public List<ResolveInfo> queryIntentActivitiesAsUser(Intent intent, ResolveInfoFlags flags, int userId) {
        return queryIntentActivities(intent, flags);
    }

    public ResolveInfo resolveServiceAsUser(Intent intent, int flags, int userId) {
        return resolveService(intent, flags);
    }

    public ResolveInfo resolveServiceAsUser(Intent intent, ResolveInfoFlags flags, int userId) {
        return resolveService(intent, flags);
    }

    public List<ResolveInfo> queryIntentServicesAsUser(Intent intent, int flags, int userId) {
        return queryIntentServices(intent, flags);
    }

    public List<ResolveInfo> queryIntentServicesAsUser(Intent intent, ResolveInfoFlags flags, int userId) {
        return queryIntentServices(intent, flags);
    }

    public List<ResolveInfo> queryIntentContentProvidersAsUser(Intent intent, int flags, int userId) {
        return queryIntentContentProviders(intent, flags);
    }

    public List<ResolveInfo> queryIntentContentProvidersAsUser(Intent intent, ResolveInfoFlags flags, int userId) {
        return queryIntentContentProviders(intent, flags);
    }

    public List<ResolveInfo> queryBroadcastReceiversAsUser(Intent intent, int flags, int userId) {
        return queryBroadcastReceivers(intent, flags);
    }

    public List<ResolveInfo> queryBroadcastReceiversAsUser(Intent intent, ResolveInfoFlags flags, int userId) {
        return queryBroadcastReceivers(intent, flags);
    }

    public ProviderInfo resolveContentProviderAsUser(String providerName, int flags, int userId) {
        return resolveContentProvider(providerName, flags);
    }

    public ProviderInfo resolveContentProviderAsUser(String providerName, ComponentInfoFlags flags, int userId) {
        return resolveContentProvider(providerName, flags);
    }

    /** The first service found, as the package manager's resolveService gives it; null for none. */
    public ResolveInfo resolveService(Intent intent, int flags) {
        return resolveService(intent, ResolveInfoFlags.of(flags));
    }

    public ResolveInfo resolveService(Intent intent, ResolveInfoFlags flags) {
        List<ResolveInfo> results = queryIntentServices(intent, flags);
        return results.isEmpty() ? null : results.get(0);
    }

    /**
     * As the package manager's chooseBestActivity, without preferred activities, which Minato keeps none of: null for
     * none, the one found, the first when it ranks above the second by priority, and else the platform's chooser,
     * ResolverActivity.
     */
    public ResolveInfo resolveActivity(Intent intent, int flags) {
        return resolveActivity(intent, ResolveInfoFlags.of(flags));
    }

    public ResolveInfo resolveActivity(Intent intent, ResolveInfoFlags flags) {
        List<ResolveInfo> results = queryIntentActivities(intent, flags);
        if (results.isEmpty()) {
            return null;
        }
        ResolveInfo first = results.get(0);
        if (results.size() == 1 || first.priority != results.get(1).priority
                || first.preferredOrder != results.get(1).preferredOrder
                || first.isDefault != results.get(1).isDefault) {
            return first;
        }
        return resolver(intent, results);
    }

    /**
     * The chooser's ResolveInfo: PackageManagerService's mResolveActivity, the platform's ResolverActivity in the
     * process system:ui, as chooseBestActivity copies it, with the meta-data android.dock_home, and the app's label and
     * icon when every choice is of the intent's package [measured: intent-query-test on apk-runner-35; the other fields
     * as PackageManagerService.setPlatformPackage sets them].
     */
    private static ResolveInfo resolver(Intent intent, List<ResolveInfo> results) {
        ApplicationInfo platform = new ApplicationInfo();
        platform.packageName = "android";
        platform.uid = Process.SYSTEM_UID;
        platform.flags = ApplicationInfo.FLAG_SYSTEM;
        ActivityInfo activity = new ActivityInfo();
        activity.applicationInfo = platform;
        activity.name = "com.android.internal.app.ResolverActivity";
        activity.packageName = "android";
        activity.processName = "system:ui";
        activity.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        activity.documentLaunchMode = ActivityInfo.DOCUMENT_LAUNCH_NEVER;
        activity.flags = ActivityInfo.FLAG_EXCLUDE_FROM_RECENTS | ActivityInfo.FLAG_FINISH_ON_CLOSE_SYSTEM_DIALOGS;
        activity.theme = com.android.internal.R.style.Theme_Material_Dialog_Alert;
        activity.exported = true;
        activity.enabled = true;
        activity.resizeMode = ActivityInfo.RESIZE_MODE_RESIZEABLE;
        activity.metaData = new Bundle();
        activity.metaData.putBoolean(Intent.METADATA_DOCK_HOME, true);
        ResolveInfo info = new ResolveInfo();
        info.activityInfo = activity;
        String target = intent.getPackage();
        if (target != null && !target.isEmpty()) {
            ApplicationInfo app = results.get(0).activityInfo.applicationInfo;
            info.resolvePackageName = target;
            info.icon = app.icon;
            info.iconResourceId = app.icon;
            info.labelRes = app.labelRes;
        }
        return info;
    }

    private List<ResolveInfo> query(int kind, Intent intent, long flags) {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null) {
            return new java.util.ArrayList<>();
        }
        Application app = ActivityThread.currentApplication();
        String type = app != null ? intent.resolveTypeIfNeeded(app.getContentResolver()) : intent.getType();
        return pkg.query(kind, intent, type, flags, this::getComponentEnabledSetting);
    }

    // Providers by authority and the XML their meta-data names (step c2c1.1), as androidx's FileProvider finds its
    // paths: resolveContentProvider and then ProviderInfo.loadXmlMetaData, which asks getXml.

    public ProviderInfo resolveContentProvider(String authority, int flags) {
        return resolveContentProvider(authority, ComponentInfoFlags.of(flags));
    }

    public ProviderInfo resolveContentProvider(String authority, ComponentInfoFlags flags) {
        ManifestPackage pkg = mThread.getManifestPackage();
        return pkg != null ? pkg.providerForAuthority(authority, flags.getValue()) : null;
    }

    /**
     * As Android's: the XML resource of a package, null for an unknown package, and null with a warning when the
     * resource cannot be read. The app's own Resources answer, as getResourcesForApplication gives them.
     */
    public XmlResourceParser getXml(String packageName, int resid, ApplicationInfo appInfo) {
        LoadedApk loaded = mThread.getLoadedApk();
        ManifestPackage pkg = mThread.getManifestPackage();
        if (loaded == null || pkg == null || !pkg.packageName.equals(packageName)) {
            return null;
        }
        try {
            return loaded.getResources().getXml(resid);
        } catch (RuntimeException e) {
            Log.w("PackageManager", "Failure retrieving xml 0x" + Integer.toHexString(resid) + " in package "
                    + packageName, e);
        }
        return null;
    }

    // Launch intents (the package manager stub plan, P2), as AOSP's over queryIntentActivities: an INFO activity of the
    // package first, else its launcher (or the leanback or car launcher); null when it has none, as for any other
    // package.

    public Intent getLaunchIntentForPackage(String packageName) {
        Intent intentToResolve = new Intent(Intent.ACTION_MAIN);
        intentToResolve.addCategory(Intent.CATEGORY_INFO);
        intentToResolve.setPackage(packageName);
        List<ResolveInfo> ris = queryIntentActivities(intentToResolve, 0);
        if (ris == null || ris.size() <= 0) {
            intentToResolve.removeCategory(Intent.CATEGORY_INFO);
            intentToResolve.addCategory(Intent.CATEGORY_LAUNCHER);
            intentToResolve.setPackage(packageName);
            ris = queryIntentActivities(intentToResolve, 0);
        }
        if (ris == null || ris.size() <= 0) {
            return null;
        }
        Intent intent = new Intent(intentToResolve);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.setClassName(ris.get(0).activityInfo.packageName, ris.get(0).activityInfo.name);
        return intent;
    }

    public Intent getLeanbackLaunchIntentForPackage(String packageName) {
        return getLaunchIntentForPackageAndCategory(packageName, Intent.CATEGORY_LEANBACK_LAUNCHER);
    }

    public Intent getCarLaunchIntentForPackage(String packageName) {
        return getLaunchIntentForPackageAndCategory(packageName, Intent.CATEGORY_CAR_LAUNCHER);
    }

    private Intent getLaunchIntentForPackageAndCategory(String packageName, String category) {
        Intent intentToResolve = new Intent(Intent.ACTION_MAIN);
        intentToResolve.addCategory(category);
        intentToResolve.setPackage(packageName);
        List<ResolveInfo> ris = queryIntentActivities(intentToResolve, 0);
        if (ris == null || ris.size() <= 0) {
            return null;
        }
        Intent intent = new Intent(intentToResolve);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.setClassName(ris.get(0).activityInfo.packageName, ris.get(0).activityInfo.name);
        return intent;
    }

    // Resources (the package manager stub plan, P1): the app's own package's, as AOSP's asks ResourcesManager for them
    // with the package's paths; for any other package getApplicationInfo or getActivityInfo throws, and an
    // ApplicationInfo of another package has no Resources here, as AOSP's throws when ResourcesManager has none. The
    // "system" package (SystemUI's context) and "android" are unknown, as decision 7 has every package but the app's.

    public Resources getResourcesForActivity(ComponentName activityName) throws NameNotFoundException {
        return getResourcesForApplication(
                getActivityInfo(activityName, PackageManager.GET_SHARED_LIBRARY_FILES).applicationInfo);
    }

    public Resources getResourcesForApplication(ApplicationInfo app) throws NameNotFoundException {
        return getResourcesForApplication(app, null);
    }

    public Resources getResourcesForApplication(ApplicationInfo app, Configuration configuration)
            throws NameNotFoundException {
        if (ANDROID.equals(app.packageName)) {
            return androidResources(configuration);
        }
        LoadedApk loaded = mThread.getLoadedApk();
        ManifestPackage pkg = mThread.getManifestPackage();
        if (loaded == null || pkg == null || !pkg.packageName.equals(app.packageName)) {
            throw new NameNotFoundException("Unable to open " + app.publicSourceDir);
        }
        return loaded.createApplicationResources(configuration);
    }

    /**
     * framework-res's Resources for "android" (P2), new at each call with an AssetManager of their own, which
     * AssetManager.Builder starts with the system's framework-res, and the system's configuration updated from the
     * one given [verified: package-answers-test on apk-runner-35].
     */
    @SuppressWarnings("deprecation") // Resources' public constructor, as LoadedApk's.
    private static Resources androidResources(Configuration configuration) {
        Resources system = Resources.getSystem();
        Configuration config = new Configuration(system.getConfiguration());
        if (configuration != null) {
            config.updateFrom(configuration);
        }
        return new Resources(new AssetManager.Builder().build(), system.getDisplayMetrics(), config);
    }

    public Resources getResourcesForApplication(String appPackageName) throws NameNotFoundException {
        return getResourcesForApplication(getApplicationInfo(appPackageName, PackageManager.GET_SHARED_LIBRARY_FILES));
    }

    /** As AOSP's, for the one user (PackageManager's hidden abstract method). */
    public Resources getResourcesForApplicationAsUser(String appPackageName, int userId) throws NameNotFoundException {
        if (userId < 0) {
            throw new IllegalArgumentException("Call does not support special user #" + userId);
        }
        try {
            return getResourcesForApplication(getApplicationInfo(appPackageName,
                    PackageManager.GET_SHARED_LIBRARY_FILES));
        } catch (NameNotFoundException e) {
            throw new NameNotFoundException("Package " + appPackageName + " doesn't exist");
        }
    }

    // Permissions (b3.5b1): the app's own and the platform's, answered by PermissionPolicy (ADR-0045 decision 6).

    public PermissionInfo getPermissionInfo(String permName, int flags) throws NameNotFoundException {
        ManifestPackage pkg = mThread.getManifestPackage();
        PermissionInfo info = pkg != null ? pkg.permissionInfo(permName, flags) : null;
        if (info == null) {
            throw new NameNotFoundException(permName);
        }
        return info;
    }

    /** The app's own grants; any other package holds nothing, as Android answers for a package it does not know. */
    // The installer (gap 1's batch 2, the Unity test APK's start asks it): Minato installs the app, no store does, so the app's own
    // package has none, as Android 15 answers for an app that adb installed [verified: package-manager-test on
    // apk-runner-35]; any other package is unknown, for which PackageManagerService throws.

    public String getInstallerPackageName(String packageName) {
        try {
            own(packageName);
        } catch (NameNotFoundException e) {
            throw new IllegalArgumentException("Unknown package: " + packageName);
        }
        return null;
    }

    // A component's enabled state (gap 9, WorkManager's start enables its job service and receivers): kept for the
    // process, where PackageManagerService keeps it in its package settings, which Minato has none of. The default
    // state keeps nothing. The app's own components only; another package is unknown [verified: jobs-test on
    // apk-runner-35]. Intent resolution reads the state (step SV1), and so do service starts (step SV2); getXInfo does
    // not yet. One map for the process, as every context's package manager sees the same settings on Android.
    private static final ArrayMap<ComponentName, Integer> sComponentStates = new ArrayMap<>();

    public void setComponentEnabledSetting(ComponentName componentName, int newState, int flags) {
        checkOwnComponent(componentName);
        if (!hasComponent(componentName)) {
            throw new IllegalArgumentException("Component class " + componentName.getClassName()
                    + " does not exist in " + componentName.getPackageName());
        }
        synchronized (sComponentStates) {
            if (newState == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT) {
                sComponentStates.remove(componentName);
            } else {
                sComponentStates.put(componentName, newState);
            }
        }
    }

    public int getComponentEnabledSetting(ComponentName componentName) {
        checkOwnComponent(componentName);
        return componentState(componentName);
    }

    /** @hide A component's state as the app set it, for the activity manager's service starts (step SV2). */
    static int componentState(ComponentName componentName) {
        synchronized (sComponentStates) {
            Integer state = sComponentStates.get(componentName);
            return state == null ? PackageManager.COMPONENT_ENABLED_STATE_DEFAULT : state;
        }
    }

    private void checkOwnComponent(ComponentName componentName) {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || !pkg.packageName.equals(componentName.getPackageName())) {
            throw new IllegalArgumentException("Unknown component: " + componentName);
        }
    }

    /** Whether the manifest declares the component, enabled or not: PackageManagerService looks it up with MATCH_DISABLED_COMPONENTS. */
    private boolean hasComponent(ComponentName componentName) {
        try {
            getActivityInfo(componentName, PackageManager.MATCH_DISABLED_COMPONENTS);
            return true;
        } catch (NameNotFoundException e) {
            // Not an activity.
        }
        try {
            getReceiverInfo(componentName, PackageManager.MATCH_DISABLED_COMPONENTS);
            return true;
        } catch (NameNotFoundException e) {
            // Not a receiver.
        }
        try {
            getServiceInfo(componentName, PackageManager.MATCH_DISABLED_COMPONENTS);
            return true;
        } catch (NameNotFoundException e) {
            // Not a service.
        }
        try {
            getProviderInfo(componentName, PackageManager.MATCH_DISABLED_COMPONENTS);
            return true;
        } catch (NameNotFoundException e) {
            return false;
        }
    }

    public int checkPermission(String permName, String packageName) {
        ManifestPackage pkg = mThread.getManifestPackage();
        return pkg != null && pkg.packageName.equals(packageName) && pkg.granted(permName)
                ? PERMISSION_GRANTED : PERMISSION_DENIED;
    }

    // Signatures (b3.5b1), from the import's signers.json: the app's own; any other package is unknown.

    public int checkSignatures(String packageName1, String packageName2) {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || !pkg.packageName.equals(packageName1) || !pkg.packageName.equals(packageName2)) {
            return SIGNATURE_UNKNOWN_PACKAGE;
        }
        return ownSignatures(pkg);
    }

    public int checkSignatures(int uid1, int uid2) {
        ManifestPackage pkg = mThread.getManifestPackage();
        if (pkg == null || uid1 != android.os.Process.myUid() || uid2 != uid1) {
            return SIGNATURE_UNKNOWN_PACKAGE;
        }
        return ownSignatures(pkg);
    }

    /** A package compared with itself, as PackageManagerServiceUtils.compareSignatures compares two. */
    private static int ownSignatures(ManifestPackage pkg) {
        return pkg.signingDetails().hasSignatures() ? SIGNATURE_MATCH : SIGNATURE_NEITHER_SIGNED;
    }

    public boolean hasSigningCertificate(String packageName, byte[] certificate, int type) {
        ManifestPackage pkg = mThread.getManifestPackage();
        return pkg != null && pkg.packageName.equals(packageName) && hasCertificate(pkg, certificate, type);
    }

    public boolean hasSigningCertificate(int uid, byte[] certificate, int type) {
        ManifestPackage pkg = mThread.getManifestPackage();
        return pkg != null && uid == android.os.Process.myUid() && hasCertificate(pkg, certificate, type);
    }

    /**
     * As the package manager service checks one: the signer or a key of its lineage, as raw X.509 or SHA-256. The
     * SHA-256 form digests through the JCA, which the guest's boot jars have no provider for yet (an open item of
     * ADR-0039): its error is logged, then thrown on.
     */
    private static boolean hasCertificate(ManifestPackage pkg, byte[] certificate, int type) {
        SigningDetails details = pkg.signingDetails();
        switch (type) {
            case CERT_INPUT_RAW_X509: return details.hasCertificate(certificate);
            case CERT_INPUT_SHA256:
                try {
                    return details.hasSha256Certificate(certificate);
                } catch (Error e) {
                    Log.w("PackageManager", "hasSigningCertificate(CERT_INPUT_SHA256) needs a JCA provider, which the"
                            + " guest has none of", e);
                    throw e;
                }
            default: return false;
        }
    }

    // System features (b3.5b1): Minato's table (ADR-0045 decision 4), Vulkan's with the session's driver.

    public boolean hasSystemFeature(String name) {
        return hasSystemFeature(name, 0);
    }

    public boolean hasSystemFeature(String name, int version) {
        return SystemFeatures.has(name, version, ManifestPackage.vulkan());
    }

    private PackageInstaller mInstaller;

    /**
     * As AOSP's: the one installer of this package manager, over the app's own sessions (the Unity test APK resolve list,
     * batch E), which LocalPackageInstaller keeps in memory, with their files below the app's cache folder.
     */
    public PackageInstaller getPackageInstaller() {
        synchronized (this) {
            if (mInstaller == null) {
                ManifestPackage pkg = mThread.getManifestPackage();
                File staging = new File(pkg.applicationInfo(0).dataDir, "cache/package-installer");
                mInstaller = new PackageInstaller(new LocalPackageInstaller(pkg.packageName, staging,
                        UserHandle.myUserId()), pkg.packageName, null, UserHandle.myUserId());
            }
            return mInstaller;
        }
    }

    /** As the package manager service lists them: the features in an ArrayMap's order, then the OpenGL ES version. */
    public FeatureInfo[] getSystemAvailableFeatures() {
        ArrayMap<String, FeatureInfo> features = new ArrayMap<>();
        for (Map.Entry<String, Integer> feature : SystemFeatures.available(ManifestPackage.vulkan()).entrySet()) {
            FeatureInfo info = new FeatureInfo();
            info.name = feature.getKey();
            info.version = feature.getValue();
            features.put(info.name, info);
        }
        FeatureInfo[] all = new FeatureInfo[features.size() + 1];
        for (int i = 0; i < features.size(); i++) {
            all[i] = features.valueAt(i);
        }
        FeatureInfo gles = new FeatureInfo();
        gles.reqGlEsVersion = SystemProperties.getInt("ro.opengles.version", FeatureInfo.GL_ES_VERSION_UNDEFINED);
        all[features.size()] = gles;
        return all;
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

    // Drawables (the package manager stub plan, P3), as AOSP's: the icon, logo and banner of an application or an
    // activity come from its package's Resources, the application's for an activity without its own, and the default
    // icon for an item without one. The one user is the owner, who has no badge, so a badge call returns its input,
    // as AOSP's does for a user without a profile. AOSP's cache of drawable constant states, and the icon of an
    // archived package and of another user's (UserIcons), which this package manager has none of, are left out.

    private static final int DEFAULT_FLAGS = PackageManager.GET_SHARED_LIBRARY_FILES;

    public Drawable getDrawable(String packageName, int resId, ApplicationInfo appInfo) {
        if (appInfo == null) {
            try {
                appInfo = getApplicationInfo(packageName, DEFAULT_FLAGS);
            } catch (NameNotFoundException e) {
                return null;
            }
        }
        if (resId != 0) {
            try {
                return getResourcesForApplication(appInfo).getDrawable(resId, null);
            } catch (NameNotFoundException e) {
                Log.w("PackageManager", "Failure retrieving resources for " + appInfo.packageName);
            } catch (Resources.NotFoundException e) {
                Log.w("PackageManager", "Failure retrieving resources for " + appInfo.packageName + ": "
                        + e.getMessage());
            } catch (Exception e) {
                Log.w("PackageManager", "Failure retrieving icon 0x" + Integer.toHexString(resId) + " in package "
                        + packageName, e);
            }
        }
        return null;
    }

    public Drawable getActivityIcon(ComponentName activityName) throws NameNotFoundException {
        return getActivityInfo(activityName, DEFAULT_FLAGS).loadIcon(this);
    }

    public Drawable getActivityIcon(Intent intent) throws NameNotFoundException {
        return activityOf(intent).loadIcon(this);
    }

    public Drawable getActivityBanner(ComponentName activityName) throws NameNotFoundException {
        return getActivityInfo(activityName, DEFAULT_FLAGS).loadBanner(this);
    }

    public Drawable getActivityBanner(Intent intent) throws NameNotFoundException {
        return activityOf(intent).loadBanner(this);
    }

    public Drawable getActivityLogo(ComponentName activityName) throws NameNotFoundException {
        return getActivityInfo(activityName, DEFAULT_FLAGS).loadLogo(this);
    }

    public Drawable getActivityLogo(Intent intent) throws NameNotFoundException {
        return activityOf(intent).loadLogo(this);
    }

    /** The intent's component, or what it resolves to for a default category; NameNotFoundException(its URI). */
    private ActivityInfo activityOf(Intent intent) throws NameNotFoundException {
        if (intent.getComponent() != null) {
            return getActivityInfo(intent.getComponent(), DEFAULT_FLAGS);
        }
        ResolveInfo info = resolveActivity(intent, PackageManager.MATCH_DEFAULT_ONLY);
        if (info != null) {
            return info.activityInfo;
        }
        throw new NameNotFoundException(intent.toUri(0));
    }

    public Drawable getApplicationIcon(ApplicationInfo info) {
        return info.loadIcon(this);
    }

    public Drawable getApplicationIcon(String packageName) throws NameNotFoundException {
        return getApplicationIcon(getApplicationInfo(packageName, DEFAULT_FLAGS));
    }

    public Drawable getApplicationBanner(ApplicationInfo info) {
        return info.loadBanner(this);
    }

    public Drawable getApplicationBanner(String packageName) throws NameNotFoundException {
        return getApplicationBanner(getApplicationInfo(packageName, DEFAULT_FLAGS));
    }

    public Drawable getApplicationLogo(ApplicationInfo info) {
        return info.loadLogo(this);
    }

    public Drawable getApplicationLogo(String packageName) throws NameNotFoundException {
        return getApplicationLogo(getApplicationInfo(packageName, DEFAULT_FLAGS));
    }

    /** The application context's theme gives the icon, as AOSP's mContext does; the system's before there is one. */
    public Drawable getDefaultActivityIcon() {
        Application app = mThread.getApplication();
        if (app != null) {
            return app.getDrawable(com.android.internal.R.drawable.sym_def_app_icon);
        }
        return Resources.getSystem().getDrawable(com.android.internal.R.drawable.sym_def_app_icon, null);
    }

    public Drawable loadItemIcon(PackageItemInfo itemInfo, ApplicationInfo appInfo) {
        return getUserBadgedIcon(loadUnbadgedItemIcon(itemInfo, appInfo), Process.myUserHandle());
    }

    public Drawable loadUnbadgedItemIcon(PackageItemInfo itemInfo, ApplicationInfo appInfo) {
        Drawable dr = null;
        if (itemInfo.packageName != null) {
            dr = getDrawable(itemInfo.packageName, itemInfo.icon, appInfo);
        }
        if (dr == null && itemInfo != appInfo && appInfo != null) {
            dr = loadUnbadgedItemIcon(appInfo, appInfo);
        }
        if (dr == null) {
            dr = itemInfo.loadDefaultIcon(this);
        }
        return dr;
    }

    public Drawable getUserBadgedIcon(Drawable icon, UserHandle user) {
        return icon;
    }

    public Drawable getUserBadgedDrawableForDensity(Drawable drawable, UserHandle user, Rect badgeLocation,
            int badgeDensity) {
        return drawable;
    }

    public Drawable getUserBadgeForDensity(UserHandle user, int density) {
        return null;
    }

    public Drawable getUserBadgeForDensityNoBackground(UserHandle user, int density) {
        return null;
    }

    public CharSequence getUserBadgedLabel(CharSequence label, UserHandle user) {
        return label;
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
