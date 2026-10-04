/*
 * The platform compat service (ADR-0039 step c2e; ADR-0045 decisions 2, 3 and 6): the IPlatformCompat of
 * system_server's PlatformCompat, local in the app's process, which ServiceManager's table gives as
 * "platform_compat". It answers an app as Android 15's answers one: every method that IPlatformCompat.aidl annotates
 * with @EnforcePermission throws SecurityException with PermissionEnforcer's message, since each needs a
 * signature|privileged permission that an app never holds (core/res/AndroidManifest.xml:7758-7774). listUIChanges,
 * which needs none itself, calls listAllChanges in the service, whose READ_COMPAT_CHANGE_CONFIG check still sees the
 * app's uid (PlatformCompat.java:407-418), so it throws too. getOverrideValidator, the one method open to an app,
 * returns AIDL's default, null, where Android's gives its validator: a gap that no app on Minato's path reaches.
 * An app's own changes never come from here: ActivityThread.main gives its process the disabled ones from
 * CompatTable, as bindApplication does.
 * It is AIDL's Default class, as the display service is: a Proxy would resolve every type in IPlatformCompat's
 * signatures. Not under com.android.minato, whose classes tools/framework-host-check.sh compiles without android.*.
 */
package com.android.internal.compat;

import android.Manifest;
import android.content.pm.ApplicationInfo;
import android.os.Binder;
import android.os.IBinder;

public final class PlatformCompatService extends IPlatformCompat.Default {
    /** PermissionEnforcer's message. */
    private static final String ACCESS_DENIED = "Access denied, requires: ";

    private final Binder mBinder = new Binder();

    private PlatformCompatService() {
        mBinder.attachInterface(this, IPlatformCompat.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "platform_compat": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new PlatformCompatService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public void reportChange(long changeId, ApplicationInfo appInfo) {
        throw denied(Manifest.permission.LOG_COMPAT_CHANGE);
    }

    @Override
    public void reportChangeByPackageName(long changeId, String packageName, int userId) {
        throw denied(Manifest.permission.LOG_COMPAT_CHANGE);
    }

    @Override
    public void reportChangeByUid(long changeId, int uid) {
        throw denied(Manifest.permission.LOG_COMPAT_CHANGE);
    }

    @Override
    public boolean isChangeEnabled(long changeId, ApplicationInfo appInfo) {
        throw deniedLogAndRead();
    }

    @Override
    public boolean isChangeEnabledByPackageName(long changeId, String packageName, int userId) {
        throw deniedLogAndRead();
    }

    @Override
    public boolean isChangeEnabledByUid(long changeId, int uid) {
        throw deniedLogAndRead();
    }

    @Override
    public void setOverrides(CompatibilityChangeConfig overrides, String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public void putAllOverridesOnReleaseBuilds(CompatibilityOverridesByPackageConfig overridesByPackage) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG_ON_RELEASE_BUILD);
    }

    @Override
    public void putOverridesOnReleaseBuilds(CompatibilityOverrideConfig overrides, String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG_ON_RELEASE_BUILD);
    }

    @Override
    public void setOverridesForTest(CompatibilityChangeConfig overrides, String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public boolean clearOverride(long changeId, String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public boolean clearOverrideForTest(long changeId, String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public void removeAllOverridesOnReleaseBuilds(
            CompatibilityOverridesToRemoveByPackageConfig overridesToRemoveByPackage) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG_ON_RELEASE_BUILD);
    }

    @Override
    public void removeOverridesOnReleaseBuilds(CompatibilityOverridesToRemoveConfig overridesToRemove,
            String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG_ON_RELEASE_BUILD);
    }

    @Override
    public int enableTargetSdkChanges(String packageName, int targetSdkVersion) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public int disableTargetSdkChanges(String packageName, int targetSdkVersion) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public void clearOverrides(String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public void clearOverridesForTest(String packageName) {
        throw denied(Manifest.permission.OVERRIDE_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public CompatibilityChangeConfig getAppConfig(ApplicationInfo appInfo) {
        throw deniedLogAndRead();
    }

    @Override
    public CompatibilityChangeInfo[] listAllChanges() {
        throw denied(Manifest.permission.READ_COMPAT_CHANGE_CONFIG);
    }

    @Override
    public CompatibilityChangeInfo[] listUIChanges() {
        return listAllChanges();
    }

    private static SecurityException denied(String permission) {
        return new SecurityException(ACCESS_DENIED + permission);
    }

    /** As PermissionEnforcer.enforcePermissionAllOf gives it for the AIDL's allOf of these two, in that order. */
    private static SecurityException deniedLogAndRead() {
        return new SecurityException(ACCESS_DENIED + "allOf={" + Manifest.permission.LOG_COMPAT_CHANGE + ", "
                + Manifest.permission.READ_COMPAT_CHANGE_CONFIG + "}");
    }
}
