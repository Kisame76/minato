// The written part of DevicePolicyManager, which tools/managergen joins with
// the API's methods. Ours under AOSP's name: AOSP's 13,000 lines ask the device policy service of system_server (the
// 150-method IDevicePolicyManager and about 60 admin types), none of which Minato has, and no admin, device owner,
// profile owner or managed profile exists on Minato. The generated methods answer as a device with none of them,
// logged once as unwritten where no value is written here. Named limit: no admin policy is ever enforced; an app that
// depends on one (a kiosk or MDM app) is not a target.
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Process;

import java.util.List;

class DevicePolicyManager {
    /** @hide UnsafeStateException's operation annotation (batch H3). */
    public @interface DevicePolicyOperation {
    }

    /** @hide PreferentialNetworkServiceConfig's default (batch H2). */
    public static final boolean PREFERENTIAL_NETWORK_SERVICE_ENABLED_DEFAULT = false;

    /** @hide */
    public DevicePolicyManager(Context context) {
    }

    /** @hide ContactsInternal's: a quick contact of a managed profile, which no user of Minato's has. */
    public void startManagedQuickContact(String actualLookupKey, long actualContactId, boolean isContactIdIgnored,
            long directoryId, Intent originalIntent) {
    }

    /** @hide CalendarContract's: an event shown in a managed profile, which no user of Minato's has: nothing starts. */
    public boolean startViewCalendarEventInManagedProfile(long eventId, long startMs, long endMs, boolean allDay,
            int flags) {
        return false;
    }

    /** @hide DeviceAdminReceiver's. */
    public static @interface OperationSafetyReason {
    }

    /** @hide */
    public static final int OPERATION_SAFETY_REASON_NONE = -1;

    /** @hide DeviceAdminReceiver's. */
    public static String operationSafetyReasonToString(int reason) {
        return reason == OPERATION_SAFETY_REASON_NONE ? "OPERATION_SAFETY_REASON_NONE"
                : reason == OPERATION_SAFETY_REASON_DRIVING_DISTRACTION ? "OPERATION_SAFETY_REASON_DRIVING_DISTRACTION"
                : "UNKNOWN_SAFETY_REASON-" + reason;
    }

    /** @hide DeviceAdminReceiver's. */
    public static boolean isValidOperationSafetyReason(int reason) {
        return reason == OPERATION_SAFETY_REASON_DRIVING_DISTRACTION;
    }

    // The answers below are those of apk-runner-35 (Android 15) for an app that is no admin and holds none of the
    // permissions: no admin is active, nothing is owned, nothing is restricted, storage is encrypted per user, and the
    // calls that need an admin or a permission throw SecurityException.

    /** Whether the component is an active admin: none is. */
    public boolean isAdminActive(ComponentName who) {
        return false;
    }

    /** The active admins: none, as null. */
    public List<ComponentName> getActiveAdmins() {
        return null;
    }

    public boolean isDeviceOwnerApp(String packageName) {
        return false;
    }

    public boolean isProfileOwnerApp(String packageName) {
        return false;
    }

    public boolean getCameraDisabled(ComponentName admin) {
        return false;
    }

    public int getKeyguardDisabledFeatures(ComponentName admin) {
        return 0;
    }

    public int getPasswordQuality(ComponentName admin) {
        return 0;
    }

    /** ENCRYPTION_STATUS_ACTIVE_PER_USER: file-based encryption. */
    public int getStorageEncryptionStatus() {
        return 5;
    }

    public boolean isLockTaskPermitted(String pkg) {
        return false;
    }

    public int getPasswordComplexity() {
        throw new SecurityException("Neither user " + Process.myUid()
                + " nor current process has android.permission.REQUEST_PASSWORD_COMPLEXITY.");
    }

    public boolean isActivePasswordSufficient() {
        throw new SecurityException("Neither user " + Process.myUid()
                + " nor current process has android.permission.MANAGE_DEVICE_POLICY_LOCK_CREDENTIALS.");
    }

    public boolean isApplicationHidden(ComponentName admin, String packageName) {
        throw new SecurityException("No active admin owned by uid " + Process.myUid());
    }

    public void lockNow() {
        throw new SecurityException("No active admin owned by uid " + Process.myUid());
    }

    public void lockNow(int flags) {
        lockNow();
    }
}
