// The written part of CrossProfileApps (service inventory, bundle B): Minato has one profile, so no app can interact
// across profiles and none has a target profile [verified: services-h-test].
import android.content.Context;
import android.os.UserHandle;
import java.util.ArrayList;
import java.util.List;

class CrossProfileApps {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public CrossProfileApps(Context context) {
        mContext = context;
    }

    public boolean canInteractAcrossProfiles() {
        return false;
    }

    public boolean canRequestInteractAcrossProfiles() {
        return false;
    }

    public List<UserHandle> getTargetUserProfiles() {
        return new ArrayList<>();
    }
}
