// The written part of BugreportManager (service inventory, bundle B): the manager is there; an app holds no right to
// request a bugreport [verified: services-h-test].
import android.content.Context;

class BugreportManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public BugreportManager(Context context) {
        mContext = context;
    }
}
