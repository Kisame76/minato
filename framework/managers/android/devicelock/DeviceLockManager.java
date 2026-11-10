// The written part of DeviceLockManager (service inventory, bundle B rest): the manager is there; no financing app locks
// the device, and an app without the lock permission gets no answer [verified: services-h-test].
import android.content.Context;

class DeviceLockManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public DeviceLockManager(Context context) {
        mContext = context;
    }
}
