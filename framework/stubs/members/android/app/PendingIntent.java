// Hidden members of the public API's compile-only stub (ADR-0039 step c2c2), which tools/managergen joins with it:
// View's drag and drop wraps a clip's intent sender to ask whether it starts an activity.
import android.content.IIntentSender;

class PendingIntent {
    /** @hide */
    public PendingIntent(IIntentSender target) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isActivity() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public @interface Flags {
    }
}
