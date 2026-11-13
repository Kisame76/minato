// Hidden members of the public API's compile-only stub (widgets), which
// tools/managergen joins with it: the collection views' remote adapter (RemoteViewsAdapter), which only an app widget
// host fills; and Notification.Builder's content views (ADR-0045).
import android.app.PendingIntent;
import android.app.RemoteInput;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.res.ColorStateList;
import android.graphics.PorterDuff;
import android.net.Uri;
import android.os.CancellationSignal;
import android.view.View;
import android.view.ViewGroup;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

class RemoteViews {
    /** @hide */
    static final String EXTRA_REMOTEADAPTER_APPWIDGET_ID = "remoteAdapterAppWidgetId";

    /** @hide */
    static final String EXTRA_REMOTEADAPTER_ON_LIGHT_BACKGROUND = "remoteAdapterOnLightBackground";

    /** @hide */
    public ApplicationInfo mApplication;

    /** @hide */
    public static final int FLAG_REAPPLY_DISALLOWED = 1;

    /** @hide */
    protected RemoteViews(ApplicationInfo application, int layoutId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void addFlags(int flags) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    protected boolean shouldUseStaticFilter() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void addView(int viewId, RemoteViews nestedView, int index) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void removeAllViewsExceptId(int viewId, int viewIdToKeep) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void removeFromParent(int viewId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setIntTag(int viewId, int key, int tag) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setDrawableTint(int viewId, boolean targetBackground, int colorFilter, PorterDuff.Mode mode) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setRippleDrawableColor(int viewId, ColorStateList colorStateList) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setProgressTintList(int viewId, ColorStateList tint) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setProgressBackgroundTintList(int viewId, ColorStateList tint) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setProgressIndeterminateTintList(int viewId, ColorStateList tint) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void setRemoteInputs(int viewId, RemoteInput[] remoteInputs) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void reduceImageSizes(int maxWidth, int maxHeight) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void visitUris(Consumer<Uri> visitor) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getSequenceNumber() {
        throw new RuntimeException("stub");
    }

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
