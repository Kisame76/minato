// The written part of TextClassification (the Unity test APK retest 11: a class apps reflect over names it, so it ships as the
// API with default answers), which tools/managergen joins with the API's methods. Before: the hidden members of the
// public API's compile-only stub (ADR-0043 step d)), which tools/managergen joins with it: a selection's toolbar
// actions.
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.view.View;

class TextClassification {
    /** @hide */
    public static final TextClassification EMPTY = null;

    /** @hide */
    public static View.OnClickListener createIntentOnClickListener(PendingIntent intent) {
        return null;
    }

    /** @hide */
    public static PendingIntent createPendingIntent(Context context, Intent intent, int requestCode) {
        return null;
    }
}
