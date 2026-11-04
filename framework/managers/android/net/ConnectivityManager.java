// The written part of ConnectivityManager (ADR-0045 decision 3), which tools/managergen joins with the API's
// methods: the hidden ctor SystemServiceRegistry calls, and the host's network (ADR-0054 step d), changing ADR-0045
// decision 4's "no network"), which LocalNetwork keeps: the active network, its legacy info, capabilities and link,
// and the network callbacks, which LocalNetwork keeps for every manager, as AOSP's static map of them. A listen or
// a request hears of the active network while it satisfies the NetworkRequest, as the default network's callback
// does; a request with a timeout hears onUnavailable when none does by then. Background data is not restricted.
// ConnectivityService's ACCESS_NETWORK_STATE check comes first, with its message.
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Handler;
import com.android.internal.minato.LocalNetwork;

class ConnectivityManager {
    private static final String ACCESS_NETWORK_STATE = "android.permission.ACCESS_NETWORK_STATE";
    private static final String CHANGE_NETWORK_STATE = "android.permission.CHANGE_NETWORK_STATE";
    private static final String WRITE_SETTINGS = "android.permission.WRITE_SETTINGS";

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
        registerNetworkCallback(request, networkCallback, null);
    }

    /** As AOSP's listen: the callback hears only of networks that satisfy `request`. */
    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback, Handler handler) {
        NetworkCapabilities need = request.networkCapabilities;
        if (networkCallback == null) {
            throw new IllegalArgumentException("null NetworkCallback");
        }
        enforceAccessPermission();
        if (!LocalNetwork.register(networkCallback, handler, need, 0)) {
            throw new IllegalArgumentException("NetworkCallback was already registered");
        }
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback) {
        request(request, networkCallback, null, 0);
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback, Handler handler) {
        request(request, networkCallback, handler, 0);
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback, int timeoutMs) {
        checkTimeout(timeoutMs);
        request(request, networkCallback, null, timeoutMs);
    }

    public void requestNetwork(NetworkRequest request, NetworkCallback networkCallback, Handler handler,
            int timeoutMs) {
        checkTimeout(timeoutMs);
        request(request, networkCallback, handler, timeoutMs);
    }

    private static void checkTimeout(int timeoutMs) {
        if (timeoutMs <= 0) {
            throw new IllegalArgumentException("timeoutMs must be strictly positive.");
        }
    }

    /**
     * As AOSP's request: Minato's one network is never brought up for it, so it is a listen that, with `timeoutMs`,
     * hears onUnavailable when no network satisfies it by then. ConnectivityService's permission check of a request
     * for an unrestricted network: CHANGE_NETWORK_STATE or WRITE_SETTINGS (Settings.checkAndNoteChangeNetworkState-
     * Operation's words).
     */
    private void request(NetworkRequest request, NetworkCallback networkCallback, Handler handler, int timeoutMs) {
        NetworkCapabilities need = request.networkCapabilities;
        if (networkCallback == null) {
            throw new IllegalArgumentException("null NetworkCallback");
        }
        if (context.checkCallingOrSelfPermission(CHANGE_NETWORK_STATE) != PackageManager.PERMISSION_GRANTED
                && context.checkCallingOrSelfPermission(WRITE_SETTINGS) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(context.getOpPackageName() + " was not granted  either of these permissions: "
                    + CHANGE_NETWORK_STATE + ", " + WRITE_SETTINGS + ".");
        }
        if (!LocalNetwork.register(networkCallback, handler, need, timeoutMs)) {
            throw new IllegalArgumentException("NetworkCallback was already registered");
        }
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
        if (!LocalNetwork.register(networkCallback, handler, null, 0)) {
            throw new IllegalArgumentException("NetworkCallback was already registered");
        }
    }

    /** ConnectivityService's enforceAccessPermission. */
    private void enforceAccessPermission() {
        context.enforceCallingOrSelfPermission(ACCESS_NETWORK_STATE, "ConnectivityService");
    }
}
