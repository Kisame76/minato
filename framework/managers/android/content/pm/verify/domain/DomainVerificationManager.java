// The written part of DomainVerificationManager (service inventory, bundle B): the manager is there; no domain is
// verified for an app of Minato [verified: services-h-test].
import android.content.Context;

class DomainVerificationManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public DomainVerificationManager(Context context) {
        mContext = context;
    }
}
