// Hidden members of the public API's compile-only stub (the compatibility plan's gap 5, widgets, batch 1), which
// tools/managergen joins with it: RemoteViewsAdapter's frame for a collection's item views, which only an app widget
// host makes, and the shared element options of RemoteViews' clicks in a host (ADR-0045 gap 1, batch 2).
import android.app.ActivityOptions;
import android.content.Context;
import android.content.Intent;
import android.widget.RemoteViews;

class AppWidgetHostView {
    /** @hide */
    public void setInteractionHandler(RemoteViews.InteractionHandler handler) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setOnLightBackground(boolean onLightBackground) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    protected void applyRemoteViews(RemoteViews remoteViews, boolean useAsyncIfPossible) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setColorResourcesNoReapply(RemoteViews.ColorResources colorResources) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public ActivityOptions createSharedElementActivityOptions(int[] sharedViewIds, String[] sharedViewNames,
            Intent fillInIntent) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static class AdapterChildHostView extends AppWidgetHostView {
        public AdapterChildHostView(Context context) {
            super(context);
        }
    }
}
