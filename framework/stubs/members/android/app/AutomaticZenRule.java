// Hidden members of the public API's compile-only stub (ADR-0045 gap 1, batch 2), which tools/managergen joins with
// it: NotificationManager turns the zen rules of the old service call into automatic rules.
import android.content.ComponentName;
import android.net.Uri;
import android.service.notification.ZenPolicy;

class AutomaticZenRule {
    /** @hide */
    public AutomaticZenRule(String name, ComponentName owner, ComponentName configurationActivity, Uri conditionId,
            ZenPolicy policy, int interruptionFilter, boolean enabled, long creationTime) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setPackageName(String pkgName) {
        throw new RuntimeException("stub");
    }
}
