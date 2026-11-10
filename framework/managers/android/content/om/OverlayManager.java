// The written part of OverlayManager (service inventory, bundle B): the manager is there; Minato has no overlays to
// manage [verified: services-h-test].
import android.content.Context;

class OverlayManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public OverlayManager(Context context) {
        mContext = context;
    }
}
