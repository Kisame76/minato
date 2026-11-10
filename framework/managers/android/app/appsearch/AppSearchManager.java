// The written part of AppSearchManager (service inventory, bundle B rest): the manager is there; its sessions are not
// written yet (each logs once), as for the other managers of the inventory [verified: services-h-test].
import android.content.Context;

class AppSearchManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public AppSearchManager(Context context) {
        mContext = context;
    }
}
