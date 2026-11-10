// The written part of CredentialManager (service inventory, bundle B): the manager is there; no credential provider is
// offered to guests [verified: services-h-test].
import android.content.Context;

class CredentialManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public CredentialManager(Context context) {
        mContext = context;
    }
}
