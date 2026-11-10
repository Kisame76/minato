// The written part of CompanionDeviceManager (service inventory, bundle B): an app has no association, and the list of
// all of them is a system app's [verified: services-h-test].
import android.content.Context;
import java.util.ArrayList;
import java.util.List;

class CompanionDeviceManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public CompanionDeviceManager(Context context) {
        mContext = context;
    }

    public List<String> getAssociations() {
        return new ArrayList<>();
    }

    public List<AssociationInfo> getMyAssociations() {
        return new ArrayList<>();
    }

    /** @hide */
    public List<AssociationInfo> getAllAssociations() {
        throw new SecurityException("Caller must hold android.permission.MANAGE_COMPANION_DEVICES");
    }
}
