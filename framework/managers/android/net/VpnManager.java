// The written part of VpnManager (service inventory, bundle C part 1): the manager is there; an app has provisioned no VPN
// profile, so its state is null, as on the emulator (apk-runner-35) [verified: services-j-test].
import android.content.Context;

class VpnManager {
    // AOSP's values of the VPN types, which VpnService.prepareAndAuthorize reads (hidden: not API).
    /** @hide */
    public static final int TYPE_VPN_NONE = -1;
    /** @hide */
    public static final int TYPE_VPN_SERVICE = 1;
    /** @hide */
    public static final int TYPE_VPN_PLATFORM = 2;
    /** @hide */
    public static final int TYPE_VPN_LEGACY = 3;
    /** @hide */
    public static final int TYPE_VPN_OEM = 4;

    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public VpnManager(Context context) {
        mContext = context;
    }

    public VpnProfileState getProvisionedVpnProfileState() {
        return null;
    }
}
