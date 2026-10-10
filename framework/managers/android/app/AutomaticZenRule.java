// The written part of AutomaticZenRule (App, the Unity test APK retest 11: a class apps reflect over names it, so it ships as the
// API with default answers), which tools/managergen joins with the API's methods. Before: the hidden members of the
// public API's compile-only stub (ADR-0045 gap 1, batch 2), which tools/managergen joins with it: NotificationManager
// turns the zen rules of the old service call into automatic rules.
import android.content.ComponentName;
import android.net.Uri;
import android.service.notification.ZenPolicy;

class AutomaticZenRule {
    /** @hide */
    public AutomaticZenRule(String name, ComponentName owner, ComponentName configurationActivity, Uri conditionId,
            ZenPolicy policy, int interruptionFilter, boolean enabled, long creationTime) {
    }

    /** @hide */
    public void setPackageName(String pkgName) {
    }
}
