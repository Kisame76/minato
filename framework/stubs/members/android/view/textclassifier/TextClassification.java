// Hidden members of the public API's compile-only stub (ADR-0043 step d)), which tools/managergen joins with it:
// a selection's toolbar actions.
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.view.View;

class TextClassification {
    /** @hide */
    public static final TextClassification EMPTY = null;

    /** @hide */
    public static View.OnClickListener createIntentOnClickListener(PendingIntent intent) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static PendingIntent createPendingIntent(Context context, Intent intent, int requestCode) {
        throw new RuntimeException("stub");
    }
}
