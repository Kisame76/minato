// Hidden members of the public API's compile-only stub (the compatibility plan's gap 5, widgets, batch 1), which
// tools/managergen joins with it: the collection views' remote adapter (RemoteViewsAdapter), which only an app widget
// host fills.
import android.app.PendingIntent;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.CancellationSignal;
import android.view.View;
import android.view.ViewGroup;
import java.util.concurrent.Executor;

class RemoteViews {
    /** @hide */
    static final String EXTRA_REMOTEADAPTER_APPWIDGET_ID = "remoteAdapterAppWidgetId";

    /** @hide */
    static final String EXTRA_REMOTEADAPTER_ON_LIGHT_BACKGROUND = "remoteAdapterOnLightBackground";

    /** @hide */
    public ApplicationInfo mApplication;

    /** @hide */
    public boolean prefersAsyncApply() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int estimateMemoryUsage() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public CancellationSignal applyAsync(Context context, ViewGroup parent, Executor executor,
            OnViewAppliedListener listener) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean hasSameAppInfo(ApplicationInfo info) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public interface InteractionHandler {
        boolean onInteraction(View view, PendingIntent pendingIntent, RemoteResponse response);
    }

    /** @hide */
    public interface OnViewAppliedListener {
        default void onViewInflated(View v) {}

        void onViewApplied(View v);

        void onError(Exception e);
    }
}
