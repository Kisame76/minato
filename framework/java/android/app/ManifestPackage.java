/*
 * Ours (ADR-0039 decision 7, b3.5a): the app's own package as its manifest declares it, read once by ActivityThread as
 * the app starts, from which ApplicationPackageManager answers. The manifest is read as Android 15's package parser
 * reads it (ParsingPackageUtils and the Parsed*Utils of com.android.internal.pm, which decision 7 does not take):
 * through libandroidfw's XmlResourceParser, with every attribute through Resources.obtainAttributes and framework-res's
 * AndroidManifest* styleables, so defaults, references and value types are framework-res's. The info objects are made
 * per request as PackageInfoCommonUtils makes them, with the flags' filtering; what system_server adds at the install
 * (paths, uid, install times, FLAG_INSTALLED of the app and of its permissions) comes from the driver.
 *
 * Not read yet, each for the step that needs it: signatures (b3.5b, from the import), intent filters beyond whether
 * there are any (b3.5b), the platform's split permissions (b3.5b), <uses-library> (sharedLibraryFiles), <layout>,
 * <property>, <grant-uri-permission>, <path-permission>, featureFlag attributes (b3.5b, for framework-res's
 * manifest), splits and instrumentation.
 */
package android.app;

import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ComponentInfo;
import android.content.pm.ConfigurationInfo;
import android.content.pm.FeatureInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageItemInfo;
import android.content.pm.PackageManager;
import android.content.pm.PathPermission;
import android.content.pm.PermissionInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.os.Bundle;
import android.os.Process;
import android.util.TypedValue;
import com.android.internal.R;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;

// Deprecated attributes, fields and flags: Android's parser still reads and fills them, and apps still read them.
@SuppressWarnings("deprecation")
final class ManifestPackage {
    /** Build.VERSION.SDK_INT of Android 15, against which <uses-permission>'s SDK bounds are checked. */
    private static final int SDK = 35;
    /** ActivityTaskManager.getDefaultAppRecentsLimitStatic() on a device that is not low on RAM: 48 / 6. */
    private static final int DEFAULT_MAX_RECENTS = 8;
    /** StorageManager.UUID_DEFAULT, internal storage. */
    private static final UUID UUID_DEFAULT = UUID.fromString("41217664-9172-527a-b3d5-edabb50a7d69");
    private static final String META_LAUNCH_MODE = "android.activity.launch_mode";
    private static final String META_REMOTE_DEVICES = "android.can_display_on_remote_devices";

    final String packageName;
    /** The app's Resources over base.apk and framework-res, with the system's configuration. */
    final Resources resources;

    private final PackageInfo mPackage = new PackageInfo();
    private final ApplicationInfo mApplication = new ApplicationInfo();
    private final List<ActivityInfo> mActivities = new ArrayList<>();
    private final List<ActivityInfo> mReceivers = new ArrayList<>();
    private final List<ServiceInfo> mServices = new ArrayList<>();
    private final List<ProviderInfo> mProviders = new ArrayList<>();
    private final List<PermissionInfo> mPermissions = new ArrayList<>();
    private final List<String> mRequestedPermissions = new ArrayList<>();
    private final List<FeatureInfo> mFeatures = new ArrayList<>();
    private final List<ConfigurationInfo> mConfigurations = new ArrayList<>();

    // <supports-screens>: 1 is unset, as PackageImpl keeps them.
    private int mSmallScreens = 1, mNormalScreens = 1, mLargeScreens = 1, mXlargeScreens = 1, mResizeable = 1,
            mAnyDensity = 1;

    private ManifestPackage(String packageName, Resources resources) {
        this.packageName = packageName;
        this.resources = resources;
    }

    /**
     * Reads base.apk's manifest; the folders and the install time are the install's, as decision 6 lays them out.
     */
    static ManifestPackage read(String apk, String libraryDir, String dataDir)
            throws IOException, XmlPullParserException {
        ApkAssets apkAssets = ApkAssets.loadFromPath(apk);
        AssetManager assets = new AssetManager();
        assets.setApkAssets(new ApkAssets[] {apkAssets}, false);
        Resources system = Resources.getSystem();
        Resources res = new Resources(assets, system.getDisplayMetrics(), system.getConfiguration());
        try (XmlResourceParser parser = assets.openXmlResourceParser(assets.findCookieForPath(apk),
                "AndroidManifest.xml")) {
            int type;
            while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) {
                // The manifest's first element.
            }
            if (type != XmlPullParser.START_TAG || !"manifest".equals(parser.getName())) {
                throw new XmlPullParserException(apk + ": no <manifest>");
            }
            String name = parser.getAttributeValue(null, "package");
            if (name == null || name.isEmpty()) {
                throw new XmlPullParserException(apk + ": <manifest> has no package");
            }
            ManifestPackage pkg = new ManifestPackage(name, res);
            pkg.parseManifest(parser);
            pkg.install(apk, libraryDir, dataDir);
            return pkg;
        }
    }

    // The manifest.

    private void parseManifest(XmlResourceParser parser) throws IOException, XmlPullParserException {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifest);
        try {
            mPackage.packageName = packageName;
            mPackage.versionCode = sa.getInteger(R.styleable.AndroidManifest_versionCode, 0);
            mPackage.versionCodeMajor = sa.getInteger(R.styleable.AndroidManifest_versionCodeMajor, 0);
            mPackage.versionName = sa.getNonConfigurationString(R.styleable.AndroidManifest_versionName, 0);
            mPackage.baseRevisionCode = sa.getInteger(R.styleable.AndroidManifest_revisionCode, 0);
            mPackage.installLocation = sa.getInteger(R.styleable.AndroidManifest_installLocation,
                    PackageInfo.INSTALL_LOCATION_UNSPECIFIED);
            String sharedUserId = sa.getNonConfigurationString(R.styleable.AndroidManifest_sharedUserId, 0);
            if (sharedUserId != null && !sharedUserId.isEmpty()) {
                mPackage.sharedUserId = sharedUserId;
                mPackage.sharedUserLabel = sa.getResourceId(R.styleable.AndroidManifest_sharedUserLabel, 0);
            }
            mApplication.compileSdkVersion = sa.getInteger(R.styleable.AndroidManifest_compileSdkVersion, 0);
            mApplication.compileSdkVersionCodename = sa.getNonConfigurationString(
                    R.styleable.AndroidManifest_compileSdkVersionCodename, 0);
        } finally {
            sa.recycle();
        }
        mPackage.splitNames = new String[0];
        mPackage.splitRevisionCodes = new int[0];
        mApplication.packageName = packageName;
        // <uses-sdk> absent: min 1, target 0.
        mApplication.minSdkVersion = 1;
        mApplication.targetSdkVersion = 0;

        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1) {
                continue;
            }
            switch (parser.getName()) {
                case "uses-sdk": parseUsesSdk(parser); break;
                case "uses-permission":
                case "uses-permission-sdk-m":
                case "uses-permission-sdk-23": parseUsesPermission(parser); break;
                case "permission": parsePermission(parser); break;
                case "uses-feature": parseUsesFeature(parser); break;
                case "uses-configuration": parseUsesConfiguration(parser); break;
                case "supports-screens": parseSupportsScreens(parser); break;
                case "application": parseApplication(parser); break;
                default: break;
            }
        }
        // Implicit permissions after the explicit ones (ParsingPackageUtils.convertCompatPermissions):
        // POST_NOTIFICATIONS below targetSdk 33, and the two older ones below 4.
        int target = mApplication.targetSdkVersion;
        if (target < 33) {
            addImplicit("android.permission.POST_NOTIFICATIONS");
            if (target < 4) {
                addImplicit("android.permission.WRITE_EXTERNAL_STORAGE");
                addImplicit("android.permission.READ_PHONE_STATE");
            }
        }
        mApplication.flags |= screenFlags();
    }

    private void addImplicit(String permission) {
        if (!mRequestedPermissions.contains(permission)) {
            mRequestedPermissions.add(permission);
        }
    }

    private void parseUsesSdk(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesSdk);
        try {
            int min = sdk(sa, R.styleable.AndroidManifestUsesSdk_minSdkVersion, 1);
            mApplication.minSdkVersion = min;
            mApplication.targetSdkVersion = sdk(sa, R.styleable.AndroidManifestUsesSdk_targetSdkVersion, min);
        } finally {
            sa.recycle();
        }
    }

    /** An SDK level given as a number; a codename names a preview, which a release build refuses. */
    private static int sdk(TypedArray sa, int index, int fallback) {
        TypedValue v = sa.peekValue(index);
        if (v == null) {
            return fallback;
        }
        if (v.type == TypedValue.TYPE_STRING) {
            throw new IllegalStateException("SDK level " + v.string + " is a preview's");
        }
        return v.data;
    }

    private void parseUsesPermission(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesPermission);
        try {
            String name = sa.getNonResourceString(R.styleable.AndroidManifestUsesPermission_name);
            int max = intOnly(sa, R.styleable.AndroidManifestUsesPermission_maxSdkVersion, Integer.MAX_VALUE);
            int min = intOnly(sa, R.styleable.AndroidManifestUsesPermission_minSdkVersion, 0);
            if (name == null || SDK < min || SDK > max || mRequestedPermissions.contains(name)) {
                return;
            }
            mRequestedPermissions.add(name);
        } finally {
            sa.recycle();
        }
    }

    /** An int attribute only when it has an int type, as the parser reads <uses-permission>'s SDK bounds. */
    private static int intOnly(TypedArray sa, int index, int fallback) {
        TypedValue v = sa.peekValue(index);
        return v != null && v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT
                ? v.data : fallback;
    }

    private void parsePermission(XmlResourceParser parser) throws IOException, XmlPullParserException {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestPermission);
        PermissionInfo info = new PermissionInfo();
        try {
            item(info, sa, R.styleable.AndroidManifestPermission_name, R.styleable.AndroidManifestPermission_label,
                    R.styleable.AndroidManifestPermission_icon, R.styleable.AndroidManifestPermission_logo,
                    R.styleable.AndroidManifestPermission_banner);
            info.descriptionRes = sa.getResourceId(R.styleable.AndroidManifestPermission_description, 0);
            info.group = sa.getNonResourceString(R.styleable.AndroidManifestPermission_permissionGroup);
            info.requestRes = sa.getResourceId(R.styleable.AndroidManifestPermission_request, 0);
            info.protectionLevel = PermissionInfo.fixProtectionLevel(sa.getInt(
                    R.styleable.AndroidManifestPermission_protectionLevel, PermissionInfo.PROTECTION_NORMAL));
            // Hard and soft restriction are the platform's alone.
            info.flags = sa.getInt(R.styleable.AndroidManifestPermission_permissionFlags, 0)
                    & ~(PermissionInfo.FLAG_HARD_RESTRICTED | PermissionInfo.FLAG_SOFT_RESTRICTED);
            int max = sa.getInt(R.styleable.AndroidManifestPermission_maxSdkVersion, -1);
            if (max != -1 && max < SDK) {
                return;
            }
        } finally {
            sa.recycle();
        }
        info.metaData = children(parser, null);
        mPermissions.add(info);
    }

    private void parseUsesFeature(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesFeature);
        try {
            FeatureInfo feature = new FeatureInfo();
            feature.name = sa.getNonResourceString(R.styleable.AndroidManifestUsesFeature_name);
            feature.version = sa.getInt(R.styleable.AndroidManifestUsesFeature_version, 0);
            if (feature.name == null) {
                feature.reqGlEsVersion = sa.getInt(R.styleable.AndroidManifestUsesFeature_glEsVersion,
                        FeatureInfo.GL_ES_VERSION_UNDEFINED);
            }
            if (sa.getBoolean(R.styleable.AndroidManifestUsesFeature_required, true)) {
                feature.flags |= FeatureInfo.FLAG_REQUIRED;
            }
            mFeatures.add(feature);
            if (feature.name == null) {
                ConfigurationInfo configuration = new ConfigurationInfo();
                configuration.reqGlEsVersion = feature.reqGlEsVersion;
                mConfigurations.add(configuration);
            }
        } finally {
            sa.recycle();
        }
    }

    private void parseUsesConfiguration(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestUsesConfiguration);
        try {
            ConfigurationInfo info = new ConfigurationInfo();
            info.reqTouchScreen = sa.getInt(R.styleable.AndroidManifestUsesConfiguration_reqTouchScreen, 0);
            info.reqKeyboardType = sa.getInt(R.styleable.AndroidManifestUsesConfiguration_reqKeyboardType, 0);
            if (sa.getBoolean(R.styleable.AndroidManifestUsesConfiguration_reqHardKeyboard, false)) {
                info.reqInputFeatures |= ConfigurationInfo.INPUT_FEATURE_HARD_KEYBOARD;
            }
            info.reqNavigation = sa.getInt(R.styleable.AndroidManifestUsesConfiguration_reqNavigation, 0);
            if (sa.getBoolean(R.styleable.AndroidManifestUsesConfiguration_reqFiveWayNav, false)) {
                info.reqInputFeatures |= ConfigurationInfo.INPUT_FEATURE_FIVE_WAY_NAV;
            }
            mConfigurations.add(info);
        } finally {
            sa.recycle();
        }
    }

    private void parseSupportsScreens(XmlResourceParser parser) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestSupportsScreens);
        try {
            mApplication.requiresSmallestWidthDp = sa.getInt(
                    R.styleable.AndroidManifestSupportsScreens_requiresSmallestWidthDp, 0);
            mApplication.compatibleWidthLimitDp = sa.getInt(
                    R.styleable.AndroidManifestSupportsScreens_compatibleWidthLimitDp, 0);
            mApplication.largestWidthLimitDp = sa.getInt(
                    R.styleable.AndroidManifestSupportsScreens_largestWidthLimitDp, 0);
            // getInt of a boolean: -1 is true, 0 false; 1 stays unset.
            mSmallScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_smallScreens, 1);
            mNormalScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_normalScreens, 1);
            mLargeScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_largeScreens, 1);
            mXlargeScreens = sa.getInt(R.styleable.AndroidManifestSupportsScreens_xlargeScreens, 1);
            mResizeable = sa.getInt(R.styleable.AndroidManifestSupportsScreens_resizeable, 1);
            mAnyDensity = sa.getInt(R.styleable.AndroidManifestSupportsScreens_anyDensity, 1);
        } finally {
            sa.recycle();
        }
    }

    /** The screen bits of ApplicationInfo.flags, as PackageImpl derives them: set if unset and the target allows. */
    private int screenFlags() {
        int target = mApplication.targetSdkVersion;
        return screen(mSmallScreens, target >= 4, ApplicationInfo.FLAG_SUPPORTS_SMALL_SCREENS)
                | screen(mNormalScreens, true, ApplicationInfo.FLAG_SUPPORTS_NORMAL_SCREENS)
                | screen(mLargeScreens, target >= 4, ApplicationInfo.FLAG_SUPPORTS_LARGE_SCREENS)
                | screen(mXlargeScreens, target >= 9, ApplicationInfo.FLAG_SUPPORTS_XLARGE_SCREENS)
                | screen(mResizeable, target >= 4, ApplicationInfo.FLAG_RESIZEABLE_FOR_SCREENS)
                | screen(mAnyDensity, target >= 4, ApplicationInfo.FLAG_SUPPORTS_SCREEN_DENSITIES);
    }

    private static int screen(int value, boolean byTarget, int flag) {
        return (value == 1 ? byTarget : value < 0) ? flag : 0;
    }

    // The application.

    private void parseApplication(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ApplicationInfo ai = mApplication;
        int target = ai.targetSdkVersion;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestApplication);
        try {
            ai.className = className(sa.getNonConfigurationString(R.styleable.AndroidManifestApplication_name, 0));
            ai.name = ai.className;
            label(ai, sa, R.styleable.AndroidManifestApplication_label);
            ai.icon = sa.getResourceId(R.styleable.AndroidManifestApplication_icon, 0);
            ai.logo = sa.getResourceId(R.styleable.AndroidManifestApplication_logo, 0);
            ai.banner = sa.getResourceId(R.styleable.AndroidManifestApplication_banner, 0);
            ai.descriptionRes = sa.getResourceId(R.styleable.AndroidManifestApplication_description, 0);
            ai.theme = sa.getResourceId(R.styleable.AndroidManifestApplication_theme, 0);
            ai.manageSpaceActivityName = className(sa.getNonConfigurationString(
                    R.styleable.AndroidManifestApplication_manageSpaceActivity, Configuration.NATIVE_CONFIG_VERSION));
            ai.permission = sa.getNonConfigurationString(R.styleable.AndroidManifestApplication_permission, 0);
            ai.uiOptions = sa.getInt(R.styleable.AndroidManifestApplication_uiOptions, 0);
            ai.category = sa.getInt(R.styleable.AndroidManifestApplication_appCategory,
                    ApplicationInfo.CATEGORY_UNDEFINED);
            ai.enabled = sa.getBoolean(R.styleable.AndroidManifestApplication_enabled, true);
            ai.appComponentFactory = className(sa.getNonResourceString(
                    R.styleable.AndroidManifestApplication_appComponentFactory));
            ai.taskAffinity = target >= 8 ? taskAffinity(packageName, sa.getNonConfigurationString(
                    R.styleable.AndroidManifestApplication_taskAffinity, Configuration.NATIVE_CONFIG_VERSION))
                    : null;
            ai.processName = processName(null, sa.getNonConfigurationString(
                    R.styleable.AndroidManifestApplication_process, Configuration.NATIVE_CONFIG_VERSION));
            if (ai.processName == null) {
                ai.processName = packageName;
            }

            int flags = 0;
            boolean allowBackup = sa.getBoolean(R.styleable.AndroidManifestApplication_allowBackup, true);
            if (allowBackup) {
                flags |= ApplicationInfo.FLAG_ALLOW_BACKUP;
                ai.backupAgentName = className(sa.getNonConfigurationString(
                        R.styleable.AndroidManifestApplication_backupAgent, 0));
                if (ai.backupAgentName != null) {
                    flags |= flag(sa, R.styleable.AndroidManifestApplication_killAfterRestore, true,
                            ApplicationInfo.FLAG_KILL_AFTER_RESTORE)
                            | flag(sa, R.styleable.AndroidManifestApplication_restoreAnyVersion, false,
                                    ApplicationInfo.FLAG_RESTORE_ANY_VERSION)
                            | flag(sa, R.styleable.AndroidManifestApplication_fullBackupOnly, false,
                                    ApplicationInfo.FLAG_FULL_BACKUP_ONLY);
                }
            }
            flags |= flag(sa, R.styleable.AndroidManifestApplication_debuggable, false,
                    ApplicationInfo.FLAG_DEBUGGABLE)
                    | flag(sa, R.styleable.AndroidManifestApplication_hasCode, true, ApplicationInfo.FLAG_HAS_CODE)
                    | flag(sa, R.styleable.AndroidManifestApplication_allowTaskReparenting, false,
                            ApplicationInfo.FLAG_ALLOW_TASK_REPARENTING)
                    | flag(sa, R.styleable.AndroidManifestApplication_allowClearUserData, true,
                            ApplicationInfo.FLAG_ALLOW_CLEAR_USER_DATA)
                    | flag(sa, R.styleable.AndroidManifestApplication_testOnly, false, ApplicationInfo.FLAG_TEST_ONLY)
                    | flag(sa, R.styleable.AndroidManifestApplication_vmSafeMode, false,
                            ApplicationInfo.FLAG_VM_SAFE_MODE)
                    | flag(sa, R.styleable.AndroidManifestApplication_largeHeap, false, ApplicationInfo.FLAG_LARGE_HEAP)
                    | flag(sa, R.styleable.AndroidManifestApplication_supportsRtl, false,
                            ApplicationInfo.FLAG_SUPPORTS_RTL)
                    | flag(sa, R.styleable.AndroidManifestApplication_isGame, false, ApplicationInfo.FLAG_IS_GAME)
                    | flag(sa, R.styleable.AndroidManifestApplication_usesCleartextTraffic, target < 28,
                            ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC)
                    | flag(sa, R.styleable.AndroidManifestApplication_extractNativeLibs, true,
                            ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS)
                    | flag(sa, R.styleable.AndroidManifestApplication_hardwareAccelerated, target >= 14,
                            ApplicationInfo.FLAG_HARDWARE_ACCELERATED)
                    | flag(sa, R.styleable.AndroidManifestApplication_multiArch, false, ApplicationInfo.FLAG_MULTIARCH);
            // FLAG_PERSISTENT is the system's alone (ScanPackageUtils.applyPolicy clears it for an app).
            ai.flags = flags;
        } finally {
            sa.recycle();
        }

        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1) {
                continue;
            }
            switch (parser.getName()) {
                case "meta-data": ai.metaData = metaData(parser, ai.metaData); break;
                case "activity": mActivities.add(parseActivity(parser, false)); break;
                case "receiver": mReceivers.add(parseActivity(parser, true)); break;
                case "activity-alias": mActivities.add(parseAlias(parser)); break;
                case "service": mServices.add(parseService(parser)); break;
                case "provider": mProviders.add(parseProvider(parser)); break;
                default: break;
            }
        }
    }

    // The components.

    private ActivityInfo parseActivity(XmlResourceParser parser, boolean receiver)
            throws IOException, XmlPullParserException {
        ApplicationInfo app = mApplication;
        ActivityInfo info = new ActivityInfo();
        boolean exportedSet;
        boolean exported;
        boolean canDisplayOnRemoteDevices;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestActivity);
        try {
            mainComponent(info, sa, R.styleable.AndroidManifestActivity_name, R.styleable.AndroidManifestActivity_label,
                    R.styleable.AndroidManifestActivity_icon, R.styleable.AndroidManifestActivity_logo,
                    R.styleable.AndroidManifestActivity_banner, R.styleable.AndroidManifestActivity_description,
                    R.styleable.AndroidManifestActivity_process, R.styleable.AndroidManifestActivity_enabled,
                    R.styleable.AndroidManifestActivity_directBootAware, R.styleable.AndroidManifestActivity_splitName,
                    R.styleable.AndroidManifestActivity_attributionTags);
            info.theme = sa.getResourceId(R.styleable.AndroidManifestActivity_theme, 0);
            info.uiOptions = sa.getInt(R.styleable.AndroidManifestActivity_uiOptions, app.uiOptions);
            int flags = flag(sa, R.styleable.AndroidManifestActivity_allowTaskReparenting,
                    (app.flags & ApplicationInfo.FLAG_ALLOW_TASK_REPARENTING) != 0,
                    ActivityInfo.FLAG_ALLOW_TASK_REPARENTING)
                    | flag(sa, R.styleable.AndroidManifestActivity_alwaysRetainTaskState, false,
                            ActivityInfo.FLAG_ALWAYS_RETAIN_TASK_STATE)
                    | flag(sa, R.styleable.AndroidManifestActivity_clearTaskOnLaunch, false,
                            ActivityInfo.FLAG_CLEAR_TASK_ON_LAUNCH)
                    | flag(sa, R.styleable.AndroidManifestActivity_excludeFromRecents, false,
                            ActivityInfo.FLAG_EXCLUDE_FROM_RECENTS)
                    | flag(sa, R.styleable.AndroidManifestActivity_finishOnCloseSystemDialogs, false,
                            ActivityInfo.FLAG_FINISH_ON_CLOSE_SYSTEM_DIALOGS)
                    | flag(sa, R.styleable.AndroidManifestActivity_finishOnTaskLaunch, false,
                            ActivityInfo.FLAG_FINISH_ON_TASK_LAUNCH)
                    | flag(sa, R.styleable.AndroidManifestActivity_immersive, false, ActivityInfo.FLAG_IMMERSIVE)
                    | flag(sa, R.styleable.AndroidManifestActivity_multiprocess, false,
                            ActivityInfo.FLAG_MULTIPROCESS)
                    | flag(sa, R.styleable.AndroidManifestActivity_noHistory, false, ActivityInfo.FLAG_NO_HISTORY)
                    | flag(sa, R.styleable.AndroidManifestActivity_showForAllUsers, false,
                            ActivityInfo.FLAG_SHOW_FOR_ALL_USERS)
                    | flag(sa, R.styleable.AndroidManifestActivity_showOnLockScreen, false,
                            ActivityInfo.FLAG_SHOW_FOR_ALL_USERS)
                    | flag(sa, R.styleable.AndroidManifestActivity_stateNotNeeded, false,
                            ActivityInfo.FLAG_STATE_NOT_NEEDED)
                    | flag(sa, R.styleable.AndroidManifestActivity_systemUserOnly, false,
                            ActivityInfo.FLAG_SYSTEM_USER_ONLY);
            if (!receiver) {
                flags |= flag(sa, R.styleable.AndroidManifestActivity_hardwareAccelerated,
                        (app.flags & ApplicationInfo.FLAG_HARDWARE_ACCELERATED) != 0,
                        ActivityInfo.FLAG_HARDWARE_ACCELERATED)
                        | flag(sa, R.styleable.AndroidManifestActivity_allowEmbedded, false,
                                ActivityInfo.FLAG_ALLOW_EMBEDDED)
                        | flag(sa, R.styleable.AndroidManifestActivity_alwaysFocusable, false,
                                ActivityInfo.FLAG_ALWAYS_FOCUSABLE)
                        | flag(sa, R.styleable.AndroidManifestActivity_autoRemoveFromRecents, false,
                                ActivityInfo.FLAG_AUTO_REMOVE_FROM_RECENTS)
                        | flag(sa, R.styleable.AndroidManifestActivity_relinquishTaskIdentity, false,
                                ActivityInfo.FLAG_RELINQUISH_TASK_IDENTITY)
                        | flag(sa, R.styleable.AndroidManifestActivity_resumeWhilePausing, false,
                                ActivityInfo.FLAG_RESUME_WHILE_PAUSING)
                        | flag(sa, R.styleable.AndroidManifestActivity_showWhenLocked, false,
                                ActivityInfo.FLAG_SHOW_WHEN_LOCKED)
                        | flag(sa, R.styleable.AndroidManifestActivity_supportsPictureInPicture, false,
                                ActivityInfo.FLAG_SUPPORTS_PICTURE_IN_PICTURE)
                        | flag(sa, R.styleable.AndroidManifestActivity_turnScreenOn, false,
                                ActivityInfo.FLAG_TURN_SCREEN_ON)
                        | flag(sa, R.styleable.AndroidManifestActivity_preferMinimalPostProcessing, false,
                                ActivityInfo.FLAG_PREFER_MINIMAL_POST_PROCESSING)
                        | flag(sa, R.styleable.AndroidManifestActivity_allowUntrustedActivityEmbedding, false,
                                ActivityInfo.FLAG_ALLOW_UNTRUSTED_ACTIVITY_EMBEDDING);
                info.colorMode = sa.getInt(R.styleable.AndroidManifestActivity_colorMode,
                        ActivityInfo.COLOR_MODE_DEFAULT);
                info.documentLaunchMode = sa.getInt(R.styleable.AndroidManifestActivity_documentLaunchMode,
                        ActivityInfo.DOCUMENT_LAUNCH_NONE);
                info.launchMode = sa.getInt(R.styleable.AndroidManifestActivity_launchMode,
                        ActivityInfo.LAUNCH_MULTIPLE);
                info.lockTaskLaunchMode = sa.getInt(R.styleable.AndroidManifestActivity_lockTaskMode, 0);
                info.maxRecents = sa.getInt(R.styleable.AndroidManifestActivity_maxRecents, DEFAULT_MAX_RECENTS);
                info.persistableMode = sa.getInteger(R.styleable.AndroidManifestActivity_persistableMode,
                        ActivityInfo.PERSIST_ROOT_ONLY);
                info.requestedVrComponent = sa.getString(R.styleable.AndroidManifestActivity_enableVrMode);
                info.rotationAnimation = sa.getInt(R.styleable.AndroidManifestActivity_rotationAnimation, -1);
                info.softInputMode = sa.getInt(R.styleable.AndroidManifestActivity_windowSoftInputMode, 0);
                // MCC and MNC unless recreateOnConfigChanges names them (ParsedActivityUtils.getActivityConfigChanges).
                info.configChanges = sa.getInt(R.styleable.AndroidManifestActivity_configChanges, 0)
                        | (~sa.getInt(R.styleable.AndroidManifestActivity_recreateOnConfigChanges, 0)
                                & (ActivityInfo.CONFIG_MCC | ActivityInfo.CONFIG_MNC));
                info.screenOrientation = sa.getInt(R.styleable.AndroidManifestActivity_screenOrientation,
                        ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            } else {
                info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
                info.configChanges = 0;
                info.screenOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
                info.rotationAnimation = -1;
                flags |= flag(sa, R.styleable.AndroidManifestActivity_singleUser, false, ActivityInfo.FLAG_SINGLE_USER);
            }
            if (sa.getBoolean(R.styleable.AndroidManifestActivity_visibleToInstantApps, false)) {
                flags |= ActivityInfo.FLAG_VISIBLE_TO_INSTANT_APP;
            }
            info.flags = flags;
            info.taskAffinity = taskAffinity(app.taskAffinity, sa.getNonConfigurationString(
                    R.styleable.AndroidManifestActivity_taskAffinity, Configuration.NATIVE_CONFIG_VERSION));
            info.requiredDisplayCategory = sa.getNonConfigurationString(
                    R.styleable.AndroidManifestActivity_requiredDisplayCategory, 0);
            String parent = sa.getNonConfigurationString(R.styleable.AndroidManifestActivity_parentActivityName,
                    Configuration.NATIVE_CONFIG_VERSION);
            if (parent != null) {
                info.parentActivityName = className(parent);
            }
            String permission = sa.getNonConfigurationString(R.styleable.AndroidManifestActivity_permission, 0);
            info.permission = permission != null ? permission : app.permission;
            exportedSet = sa.hasValue(R.styleable.AndroidManifestActivity_exported);
            exported = sa.getBoolean(R.styleable.AndroidManifestActivity_exported, false);
            canDisplayOnRemoteDevices = sa.getBoolean(R.styleable.AndroidManifestActivity_canDisplayOnRemoteDevices,
                    true);
        } finally {
            sa.recycle();
        }
        int[] filters = {0, 0};
        info.metaData = children(parser, filters);
        // An intent filter without an action is dropped, so it does not count.
        info.exported = exportedSet ? exported : filters[0] > 0;
        Bundle meta = info.metaData;
        if (!receiver && meta != null && "singleInstancePerTask".equals(meta.getString(META_LAUNCH_MODE))) {
            info.launchMode = ActivityInfo.LAUNCH_SINGLE_INSTANCE_PER_TASK;
        }
        if (canDisplayOnRemoteDevices && (meta == null || meta.getBoolean(META_REMOTE_DEVICES, true))) {
            info.flags |= ActivityInfo.FLAG_CAN_DISPLAY_ON_REMOTE_DEVICES;
        }
        return info;
    }

    /** An alias is a copy of its target, which comes before it, with its own name and a few of its own attributes. */
    private ActivityInfo parseAlias(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ActivityInfo info;
        boolean exportedSet;
        boolean exported;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestActivityAlias);
        try {
            String target = className(sa.getNonConfigurationString(
                    R.styleable.AndroidManifestActivityAlias_targetActivity, Configuration.NATIVE_CONFIG_VERSION));
            ActivityInfo targetInfo = null;
            for (ActivityInfo activity : mActivities) {
                if (activity.targetActivity == null && activity.name.equals(target)) {
                    targetInfo = activity;
                }
            }
            if (targetInfo == null) {
                throw new IllegalStateException("<activity-alias> target " + target + " is not an earlier activity");
            }
            info = new ActivityInfo(targetInfo);
            info.targetActivity = target;
            info.metaData = null;
            info.permission = null;
            info.persistableMode = ActivityInfo.PERSIST_ROOT_ONLY;
            info.rotationAnimation = -1;
            info.colorMode = ActivityInfo.COLOR_MODE_DEFAULT;
            info.splitName = null;
            info.attributionTags = new String[0];
            info.name = className(sa.getNonConfigurationString(R.styleable.AndroidManifestActivityAlias_name, 0));
            int icon = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_icon, 0);
            if (icon != 0) {
                info.icon = icon;
                info.nonLocalizedLabel = null;
            }
            int logo = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_logo, 0);
            if (logo != 0) {
                info.logo = logo;
            }
            int banner = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_banner, 0);
            if (banner != 0) {
                info.banner = banner;
            }
            info.descriptionRes = sa.getResourceId(R.styleable.AndroidManifestActivityAlias_description, 0);
            if (sa.peekValue(R.styleable.AndroidManifestActivityAlias_label) != null) {
                info.labelRes = 0;
                info.nonLocalizedLabel = null;
                label(info, sa, R.styleable.AndroidManifestActivityAlias_label);
            }
            info.enabled = sa.getBoolean(R.styleable.AndroidManifestActivityAlias_enabled, true);
            String parent = sa.getNonConfigurationString(R.styleable.AndroidManifestActivityAlias_parentActivityName,
                    Configuration.NATIVE_CONFIG_VERSION);
            if (parent != null) {
                info.parentActivityName = className(parent);
            }
            info.permission = sa.getNonConfigurationString(R.styleable.AndroidManifestActivityAlias_permission, 0);
            exportedSet = sa.hasValue(R.styleable.AndroidManifestActivityAlias_exported);
            exported = sa.getBoolean(R.styleable.AndroidManifestActivityAlias_exported, false);
        } finally {
            sa.recycle();
        }
        int[] filters = {0, 0};
        info.metaData = children(parser, filters);
        info.exported = exportedSet ? exported : filters[0] > 0;
        return info;
    }

    private ServiceInfo parseService(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ServiceInfo info = new ServiceInfo();
        boolean exportedSet;
        boolean exported;
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestService);
        try {
            mainComponent(info, sa, R.styleable.AndroidManifestService_name, R.styleable.AndroidManifestService_label,
                    R.styleable.AndroidManifestService_icon, R.styleable.AndroidManifestService_logo,
                    R.styleable.AndroidManifestService_banner, R.styleable.AndroidManifestService_description,
                    R.styleable.AndroidManifestService_process, R.styleable.AndroidManifestService_enabled,
                    R.styleable.AndroidManifestService_directBootAware, R.styleable.AndroidManifestService_splitName,
                    R.styleable.AndroidManifestService_attributionTags);
            String permission = sa.getNonConfigurationString(R.styleable.AndroidManifestService_permission, 0);
            info.permission = permission != null ? permission : mApplication.permission;
            info.mForegroundServiceType = sa.getInt(R.styleable.AndroidManifestService_foregroundServiceType,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_NONE);
            info.flags = flag(sa, R.styleable.AndroidManifestService_stopWithTask, false, ServiceInfo.FLAG_STOP_WITH_TASK)
                    | flag(sa, R.styleable.AndroidManifestService_isolatedProcess, false,
                            ServiceInfo.FLAG_ISOLATED_PROCESS)
                    | flag(sa, R.styleable.AndroidManifestService_externalService, false,
                            ServiceInfo.FLAG_EXTERNAL_SERVICE)
                    | flag(sa, R.styleable.AndroidManifestService_useAppZygote, false,
                            ServiceInfo.FLAG_USE_APP_ZYGOTE)
                    | flag(sa, R.styleable.AndroidManifestService_allowSharedIsolatedProcess, false,
                            ServiceInfo.FLAG_ALLOW_SHARED_ISOLATED_PROCESS)
                    | flag(sa, R.styleable.AndroidManifestService_singleUser, false, ServiceInfo.FLAG_SINGLE_USER)
                    | flag(sa, R.styleable.AndroidManifestService_visibleToInstantApps, false,
                            ServiceInfo.FLAG_VISIBLE_TO_INSTANT_APP);
            exportedSet = sa.hasValue(R.styleable.AndroidManifestService_exported);
            exported = sa.getBoolean(R.styleable.AndroidManifestService_exported, false);
        } finally {
            sa.recycle();
        }
        int[] filters = {0, 0};
        info.metaData = children(parser, filters);
        // A service's intent filter counts even without an action.
        info.exported = exportedSet ? exported : filters[1] > 0;
        return info;
    }

    private ProviderInfo parseProvider(XmlResourceParser parser) throws IOException, XmlPullParserException {
        ProviderInfo info = new ProviderInfo();
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestProvider);
        try {
            mainComponent(info, sa, R.styleable.AndroidManifestProvider_name, R.styleable.AndroidManifestProvider_label,
                    R.styleable.AndroidManifestProvider_icon, R.styleable.AndroidManifestProvider_logo,
                    R.styleable.AndroidManifestProvider_banner, R.styleable.AndroidManifestProvider_description,
                    R.styleable.AndroidManifestProvider_process, R.styleable.AndroidManifestProvider_enabled,
                    R.styleable.AndroidManifestProvider_directBootAware, R.styleable.AndroidManifestProvider_splitName,
                    R.styleable.AndroidManifestProvider_attributionTags);
            info.authority = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_authorities, 0);
            if (info.authority == null || info.authority.isEmpty()) {
                throw new IllegalStateException("<provider> " + info.name + " has no authorities");
            }
            info.isSyncable = sa.getBoolean(R.styleable.AndroidManifestProvider_syncable, false);
            info.exported = sa.getBoolean(R.styleable.AndroidManifestProvider_exported,
                    mApplication.targetSdkVersion < 17);
            String permission = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_permission, 0);
            String read = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_readPermission, 0);
            String write = sa.getNonConfigurationString(R.styleable.AndroidManifestProvider_writePermission, 0);
            String fallback = permission != null ? permission : mApplication.permission;
            info.readPermission = read != null ? read : fallback;
            info.writePermission = write != null ? write : fallback;
            info.grantUriPermissions = sa.getBoolean(R.styleable.AndroidManifestProvider_grantUriPermissions, false);
            info.forceUriPermissions = sa.getBoolean(R.styleable.AndroidManifestProvider_forceUriPermissions, false);
            info.multiprocess = sa.getBoolean(R.styleable.AndroidManifestProvider_multiprocess, false);
            info.initOrder = sa.getInt(R.styleable.AndroidManifestProvider_initOrder, 0);
            info.flags = flag(sa, R.styleable.AndroidManifestProvider_singleUser, false, ProviderInfo.FLAG_SINGLE_USER)
                    | flag(sa, R.styleable.AndroidManifestProvider_visibleToInstantApps, false,
                            ProviderInfo.FLAG_VISIBLE_TO_INSTANT_APP);
        } finally {
            sa.recycle();
        }
        info.metaData = children(parser, null);
        info.pathPermissions = new PathPermission[0];
        return info;
    }

    /** What every main component reads (ParsedComponentUtils and ParsedMainComponentUtils). */
    private void mainComponent(ComponentInfo info, TypedArray sa, int name, int label, int icon, int logo, int banner,
            int description, int process, int enabled, int directBootAware, int splitName, int attributionTags) {
        item(info, sa, name, label, icon, logo, banner);
        info.descriptionRes = sa.getResourceId(description, 0);
        info.processName = processName(mApplication.processName,
                sa.getNonConfigurationString(process, Configuration.NATIVE_CONFIG_VERSION));
        info.enabled = sa.getBoolean(enabled, true);
        info.directBootAware = sa.getBoolean(directBootAware, false);
        info.splitName = sa.getNonConfigurationString(splitName, 0);
        String tags = sa.getNonConfigurationString(attributionTags, 0);
        // getAttributionTags() of a component without them is EmptyArray.STRING.
        info.attributionTags = tags != null ? tags.split("\\|") : new String[0];
    }

    /** Name, icon, logo, banner and label, as ParsedComponentUtils.parseComponent reads them. */
    private void item(PackageItemInfo info, TypedArray sa, int name, int label, int icon, int logo, int banner) {
        info.name = className(sa.getNonConfigurationString(name, 0));
        if (info.name == null) {
            throw new IllegalStateException("a component of " + packageName + " has no name");
        }
        info.packageName = packageName;
        int iconRes = sa.getResourceId(icon, 0);
        if (iconRes != 0) {
            info.icon = iconRes;
            info.nonLocalizedLabel = null;
        }
        info.logo = sa.getResourceId(logo, 0);
        info.banner = sa.getResourceId(banner, 0);
        label(info, sa, label);
    }

    private static void label(PackageItemInfo info, TypedArray sa, int index) {
        TypedValue v = sa.peekValue(index);
        if (v != null) {
            info.labelRes = v.resourceId;
            if (v.resourceId == 0) {
                info.nonLocalizedLabel = v.coerceToString();
            }
        }
    }

    /**
     * A component's children: its <meta-data> as a Bundle, null if it has none, and in filters[0] the number of
     * <intent-filter>s with an action (an activity's, receiver's or alias's count), in filters[1] of all of them.
     */
    private Bundle children(XmlResourceParser parser, int[] filters) throws IOException, XmlPullParserException {
        Bundle metaData = null;
        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type != XmlPullParser.START_TAG || parser.getDepth() != depth + 1) {
                continue;
            }
            if ("meta-data".equals(parser.getName())) {
                metaData = metaData(parser, metaData);
            } else if ("intent-filter".equals(parser.getName()) && filters != null) {
                if (hasAction(parser)) {
                    filters[0]++;
                }
                filters[1]++;
            }
        }
        return metaData;
    }

    private static boolean hasAction(XmlResourceParser parser) throws IOException, XmlPullParserException {
        boolean action = false;
        int depth = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT
                && (type != XmlPullParser.END_TAG || parser.getDepth() > depth)) {
            if (type == XmlPullParser.START_TAG && parser.getDepth() == depth + 1
                    && "action".equals(parser.getName())) {
                action = true;
            }
        }
        return action;
    }

    /** One <meta-data> into the Bundle, as ParsingPackageUtils.parseMetaData and Property.toBundle put it. */
    private Bundle metaData(XmlResourceParser parser, Bundle into) {
        TypedArray sa = resources.obtainAttributes(parser, R.styleable.AndroidManifestMetaData);
        try {
            String name = sa.getNonConfigurationString(R.styleable.AndroidManifestMetaData_name, 0);
            if (name == null) {
                throw new IllegalStateException("<meta-data> has no android:name");
            }
            Bundle bundle = into != null ? into : new Bundle();
            TypedValue v = sa.peekValue(R.styleable.AndroidManifestMetaData_resource);
            if (v != null && v.resourceId != 0) {
                bundle.putInt(name, v.resourceId);
                return bundle;
            }
            v = sa.peekValue(R.styleable.AndroidManifestMetaData_value);
            if (v == null) {
                throw new IllegalStateException("<meta-data> " + name + " has no value or resource");
            }
            if (v.type == TypedValue.TYPE_STRING) {
                CharSequence cs = v.coerceToString();
                bundle.putString(name, cs != null ? cs.toString() : null);
            } else if (v.type == TypedValue.TYPE_INT_BOOLEAN) {
                bundle.putBoolean(name, v.data != 0);
            } else if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) {
                bundle.putInt(name, v.data);
            } else if (v.type == TypedValue.TYPE_FLOAT) {
                bundle.putFloat(name, v.getFloat());
            } else {
                // A dimension or fraction: Android logs it and leaves it out.
                return into;
            }
            return bundle;
        } finally {
            sa.recycle();
        }
    }

    // Names, as ParsingUtils and ComponentParseUtils build them.

    private String className(String name) {
        if (name == null || name.isEmpty()) {
            return null;
        }
        if (name.charAt(0) == '.') {
            return packageName + name;
        }
        return name.indexOf('.') < 0 ? packageName + "." + name : name;
    }

    private String processName(String fallback, String name) {
        return name == null || name.isEmpty() ? fallback : compoundName(name);
    }

    private String taskAffinity(String fallback, String name) {
        if (name == null) {
            return fallback;
        }
        return name.isEmpty() ? null : compoundName(name);
    }

    private String compoundName(String name) {
        return name.charAt(0) == ':' ? packageName + name : name;
    }

    private static int flag(TypedArray sa, int index, boolean fallback, int flag) {
        return sa.getBoolean(index, fallback) ? flag : 0;
    }

    // The install.

    /** What the installer and system_server add: decision 6's folders, decision 3's uid, the install time. */
    private void install(String apk, String libraryDir, String dataDir) {
        ApplicationInfo ai = mApplication;
        ai.sourceDir = apk;
        ai.publicSourceDir = apk;
        ai.nativeLibraryDir = libraryDir;
        ai.nativeLibraryRootDir = new File(libraryDir).getParent();
        ai.primaryCpuAbi = "arm64-v8a";
        ai.dataDir = dataDir;
        ai.credentialProtectedDataDir = dataDir;
        ai.deviceProtectedDataDir = "/data/user_de/0/" + packageName;
        ai.storageUuid = UUID_DEFAULT;
        ai.uid = Process.myUid();
        ai.flags |= ApplicationInfo.FLAG_INSTALLED;
        for (PermissionInfo permission : mPermissions) {
            permission.flags |= PermissionInfo.FLAG_INSTALLED;
        }
        ai.setVersionCode(mPackage.getLongVersionCode());
        long installed = new File(apk).lastModified();
        mPackage.firstInstallTime = installed;
        mPackage.lastUpdateTime = installed;
    }

    // The answers, as PackageInfoCommonUtils makes them.

    PackageInfo packageInfo(long flags) {
        PackageInfo info = new PackageInfo();
        info.packageName = mPackage.packageName;
        info.splitNames = mPackage.splitNames.clone();
        info.versionCode = mPackage.versionCode;
        info.versionCodeMajor = mPackage.versionCodeMajor;
        info.baseRevisionCode = mPackage.baseRevisionCode;
        info.splitRevisionCodes = mPackage.splitRevisionCodes.clone();
        info.versionName = mPackage.versionName;
        info.sharedUserId = mPackage.sharedUserId;
        info.sharedUserLabel = mPackage.sharedUserLabel;
        info.installLocation = mPackage.installLocation;
        info.firstInstallTime = mPackage.firstInstallTime;
        info.lastUpdateTime = mPackage.lastUpdateTime;
        info.compileSdkVersion = mApplication.compileSdkVersion;
        info.compileSdkVersionCodename = mApplication.compileSdkVersionCodename;
        ApplicationInfo app = applicationInfo(flags);
        info.applicationInfo = app;
        if ((flags & PackageManager.GET_CONFIGURATIONS) != 0) {
            info.configPreferences = mConfigurations.isEmpty() ? null
                    : mConfigurations.toArray(new ConfigurationInfo[0]);
            info.reqFeatures = mFeatures.isEmpty() ? null : mFeatures.toArray(new FeatureInfo[0]);
        }
        if ((flags & PackageManager.GET_PERMISSIONS) != 0) {
            if (!mPermissions.isEmpty()) {
                info.permissions = new PermissionInfo[mPermissions.size()];
                for (int i = 0; i < info.permissions.length; i++) {
                    info.permissions[i] = permissionInfo(mPermissions.get(i), flags);
                }
            }
            if (!mRequestedPermissions.isEmpty()) {
                info.requestedPermissions = mRequestedPermissions.toArray(new String[0]);
                info.requestedPermissionsFlags = new int[info.requestedPermissions.length];
                for (int i = 0; i < info.requestedPermissionsFlags.length; i++) {
                    info.requestedPermissionsFlags[i] = PackageInfo.REQUESTED_PERMISSION_REQUIRED;
                }
            }
        }
        if ((flags & PackageManager.GET_ACTIVITIES) != 0) {
            info.activities = activities(mActivities, flags, app);
        }
        if ((flags & PackageManager.GET_RECEIVERS) != 0) {
            info.receivers = activities(mReceivers, flags, app);
        }
        if ((flags & PackageManager.GET_SERVICES) != 0) {
            List<ServiceInfo> services = new ArrayList<>();
            for (ServiceInfo service : mServices) {
                if (matches(service, flags)) {
                    services.add(serviceInfo(service, flags, app));
                }
            }
            info.services = services.isEmpty() ? null : services.toArray(new ServiceInfo[0]);
        }
        if ((flags & PackageManager.GET_PROVIDERS) != 0) {
            List<ProviderInfo> providers = new ArrayList<>();
            for (ProviderInfo provider : mProviders) {
                if (matches(provider, flags)) {
                    providers.add(providerInfo(provider, flags, app));
                }
            }
            info.providers = providers.isEmpty() ? null : providers.toArray(new ProviderInfo[0]);
        }
        return info;
    }

    ApplicationInfo applicationInfo(long flags) {
        ApplicationInfo info = new ApplicationInfo(mApplication);
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
        if ((flags & PackageManager.GET_SHARED_LIBRARY_FILES) == 0) {
            info.sharedLibraryFiles = null;
        }
        return info;
    }

    /** The activity, alias or receiver of that name, or null. */
    ActivityInfo activityInfo(ComponentName component, long flags, boolean receiver) {
        for (ActivityInfo activity : receiver ? mReceivers : mActivities) {
            if (named(activity, component) && matches(activity, flags)) {
                return activityInfo(activity, flags, applicationInfo(flags));
            }
        }
        return null;
    }

    ServiceInfo serviceInfo(ComponentName component, long flags) {
        for (ServiceInfo service : mServices) {
            if (named(service, component) && matches(service, flags)) {
                return serviceInfo(service, flags, applicationInfo(flags));
            }
        }
        return null;
    }

    ProviderInfo providerInfo(ComponentName component, long flags) {
        for (ProviderInfo provider : mProviders) {
            if (named(provider, component) && matches(provider, flags)) {
                return providerInfo(provider, flags, applicationInfo(flags));
            }
        }
        return null;
    }

    private boolean named(ComponentInfo info, ComponentName component) {
        return packageName.equals(component.getPackageName()) && info.name.equals(component.getClassName());
    }

    /** A component disabled in the manifest only with MATCH_DISABLED_COMPONENTS, as Android's user state matches. */
    private static boolean matches(ComponentInfo info, long flags) {
        return info.enabled || (flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0;
    }

    private static ActivityInfo[] activities(List<ActivityInfo> list, long flags, ApplicationInfo app) {
        List<ActivityInfo> out = new ArrayList<>();
        for (ActivityInfo activity : list) {
            if (matches(activity, flags)) {
                out.add(activityInfo(activity, flags, app));
            }
        }
        return out.isEmpty() ? null : out.toArray(new ActivityInfo[0]);
    }

    private static ActivityInfo activityInfo(ActivityInfo template, long flags, ApplicationInfo app) {
        ActivityInfo info = new ActivityInfo(template);
        component(info, flags, app);
        return info;
    }

    private static ServiceInfo serviceInfo(ServiceInfo template, long flags, ApplicationInfo app) {
        ServiceInfo info = new ServiceInfo(template);
        component(info, flags, app);
        return info;
    }

    private static ProviderInfo providerInfo(ProviderInfo template, long flags, ApplicationInfo app) {
        ProviderInfo info = new ProviderInfo(template);
        component(info, flags, app);
        if ((flags & PackageManager.GET_URI_PERMISSION_PATTERNS) == 0) {
            info.uriPermissionPatterns = null;
        }
        return info;
    }

    private static void component(ComponentInfo info, long flags, ApplicationInfo app) {
        info.applicationInfo = app;
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
    }

    private static PermissionInfo permissionInfo(PermissionInfo template, long flags) {
        PermissionInfo info = new PermissionInfo(template);
        if ((flags & PackageManager.GET_META_DATA) == 0) {
            info.metaData = null;
        }
        return info;
    }
}
