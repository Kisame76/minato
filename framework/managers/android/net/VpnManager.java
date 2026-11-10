// The written part of VpnManager (service inventory, bundle C part 1): the manager is there; an app has provisioned no VPN
// profile, so its state is null, as on the emulator (apk-runner-35) [verified: services-j-test].
import android.content.Context;

class VpnManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public VpnManager(Context context) {
        mContext = context;
    }

    public VpnProfileState getProvisionedVpnProfileState() {
        return null;
    }
}
