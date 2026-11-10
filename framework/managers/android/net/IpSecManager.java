// The written part of IpSecManager (service inventory, bundle C part 1): IPsec: the manager is there; Minato has no IPsec service, so a transform an app asks for is not made [verified: services-j-test].
import android.content.Context;

class IpSecManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public IpSecManager(Context context) {
        mContext = context;
    }
}
