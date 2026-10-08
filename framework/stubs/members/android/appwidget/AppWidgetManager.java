// Hidden members of the public API's compile-only stub (the compatibility plan's gap 5, widgets, batch 1), which
// tools/managergen joins with it: RemoteViewsAdapter binds an app widget's remote views service.
import android.app.IServiceConnection;
import android.content.Context;
import android.content.Intent;

class AppWidgetManager {
    /** @hide */
    public boolean bindRemoteViewsService(Context context, int appWidgetId, Intent intent, IServiceConnection connection,
            int flags) {
        throw new RuntimeException("stub");
    }
}
