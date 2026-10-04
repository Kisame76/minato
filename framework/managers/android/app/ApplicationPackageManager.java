// The hand file of our ApplicationPackageManager (ADR-0039 decision 7, b3.5a and b3.5b1): tools/managergen --implement
// writes the class from PackageManager's abstract methods, and the members here replace the generated ones. It
// answers from the app's manifest as ActivityThread read it, the platform's permissions and the import's signers,
// without IPackageManager.
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.SigningDetails;
import android.content.res.XmlResourceParser;
import android.os.SystemProperties;
import android.util.ArrayMap;
import android.util.Log;
import com.android.minato.pm.SystemFeatures;
import java.util.Map;

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
