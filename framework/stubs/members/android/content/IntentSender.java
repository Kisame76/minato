// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's drag and drop asks whether a clip's intent sender starts an activity. (ADR-0045 step d2) PendingIntent's
// getIntentSender makes one. (The framework breadth's batch 1) ClipData's Item asks whether its intent sender is
// immutable.
import android.os.IBinder;

class IntentSender {
    /** @hide */
    public IntentSender(IIntentSender target, IBinder allowlistToken) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public IIntentSender getTarget() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public IBinder getWhitelistToken() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isImmutable() {
        throw new RuntimeException("stub");
    }
}
