// Hidden members of the public API's compile-only stub, which tools/managergen joins with it: ContactsInternal (a taken
// AOSP file, ContactsContract's quick contact) starts Quick Contact on the managed profile through it.
import android.content.Intent;

class DevicePolicyManager {
    /** @hide */
    public void startManagedQuickContact(String actualLookupKey, long actualContactId, boolean isContactIdIgnored,
            long directoryId, Intent originalIntent) {
        throw new RuntimeException("stub");
    }
}
