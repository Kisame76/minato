// The written part of ConnectivityManager (ADR-0045 decision 3), which tools/managergen joins with the API's
// methods: the hidden ctor SystemServiceRegistry calls, and the host's network (ADR-0054 step d), changing ADR-0045
// decision 4's "no network"), which LocalNetwork keeps: the active network, its legacy info, capabilities and link,
// and the network callbacks, which LocalNetwork keeps for every manager, as AOSP's static map of them. A network
// request's callback hears of the active network as the default network's does. Background data is not restricted.
// ConnectivityService's ACCESS_NETWORK_STATE check comes first, with its message.
import android.content.Context;
import android.os.Handler;
import com.android.internal.minato.LocalNetwork;

class ConnectivityManager {
    private static final String ACCESS_NETWORK_STATE = "android.permission.ACCESS_NETWORK_STATE";

    /** AOSP's MAX_NETWORK_TYPE at android-15.0.0_r1: TYPE_TEST. */
    private static final int MAX_NETWORK_TYPE = 18;

    private final Context context;

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's ConnectivityManager takes
     * its IConnectivityManager too; ours has none.
     */
    public ConnectivityManager(Context context) {
        this.context = context;
    }

    public Network getActiveNetwork() {
        enforceAccessPermission();
        return LocalNetwork.active();
    }

    public NetworkInfo getActiveNetworkInfo() {
        enforceAccessPermission();
        return LocalNetwork.info((Network) null);
    }

    @Deprecated
    public NetworkInfo getNetworkInfo(int networkType) {
        enforceAccessPermission();
        return LocalNetwork.info(networkType);
    }

    @Deprecated
    public NetworkInfo getNetworkInfo(Network network) {
        enforceAccessPermission();
        return network == null ? null : LocalNetwork.info(network);
    }

    public NetworkCapabilities getNetworkCapabilities(Network network) {
        enforceAccessPermission();
        return LocalNetwork.capabilities(network);
    }

    public LinkProperties getLinkProperties(Network network) {
        enforceAccessPermission();
        return LocalNetwork.link(network);
    }

    public Network[] getAllNetworks() {
        enforceAccessPermission();
        return LocalNetwork.all();
    }

    public boolean isActiveNetworkMetered() {
        enforceAccessPermission();
        return LocalNetwork.metered();
    }

    /** As AOSP's: a legacy type from TYPE_MOBILE to MAX_NETWORK_TYPE. */
    @Deprecated
    public static boolean isNetworkTypeValid(int networkType) {
        return TYPE_MOBILE <= networkType && networkType <= MAX_NETWORK_TYPE;
    }

    public int getRestrictBackgroundStatus() {
        return RESTRICT_BACKGROUND_STATUS_DISABLED;
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback) {
        register(networkCallback, null);
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback, Handler handler) {
        register(networkCallback, handler);
    }

    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback) {
        register(networkCallback, null);
    }

    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback, Handler handler) {
        register(networkCallback, handler);
    }

    public void unregisterNetworkCallback(NetworkCallback networkCallback) {
        if (networkCallback == null || !LocalNetwork.unregister(networkCallback)) {
            throw new IllegalArgumentException("NetworkCallback was not registered");
        }
    }

    /** As registerDefaultNetworkCallback's path: the permission, then the callback kept once, on `handler`. */
    private void register(NetworkCallback networkCallback, Handler handler) {
        if (networkCallback == null) {
            throw new IllegalArgumentException("null NetworkCallback");
        }
        enforceAccessPermission();
        if (!LocalNetwork.register(networkCallback, handler)) {
            throw new IllegalArgumentException("NetworkCallback was already registered");
        }
    }

    /** ConnectivityService's enforceAccessPermission. */
    private void enforceAccessPermission() {
        context.enforceCallingOrSelfPermission(ACCESS_NETWORK_STATE, "ConnectivityService");
    }
}
