// The written part of ConnectivityManager (ADR-0045 decision 3), which tools/managergen joins with the API's
// methods: the hidden ctor SystemServiceRegistry calls, and decision 4's "no network" (gap 1 of the compatibility
// plan, batch 1): no active network, no networks, metered, background data not restricted, and network callbacks kept
// and never called. ConnectivityService's ACCESS_NETWORK_STATE check comes first, with its message. Gap 3 d) reports
// the real network once Runtime's DNS (b) and its path events (c) are in: App's framework side, agreed 2026-10-07.
import android.content.Context;
import android.os.Handler;
import java.util.HashSet;
import java.util.Set;

class ConnectivityManager {
    private static final String ACCESS_NETWORK_STATE = "android.permission.ACCESS_NETWORK_STATE";

    private final Context context;
    /** The callbacks registered and not unregistered, as ConnectivityManager's own map of them. */
    private final Set<NetworkCallback> callbacks = new HashSet<>();

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's ConnectivityManager takes
     * its IConnectivityManager too; ours has none.
     */
    public ConnectivityManager(Context context) {
        this.context = context;
    }

    public Network getActiveNetwork() {
        enforceAccessPermission();
        return null;
    }

    public NetworkInfo getActiveNetworkInfo() {
        enforceAccessPermission();
        return null;
    }

    public NetworkCapabilities getNetworkCapabilities(Network network) {
        enforceAccessPermission();
        return null;
    }

    public LinkProperties getLinkProperties(Network network) {
        enforceAccessPermission();
        return null;
    }

    public Network[] getAllNetworks() {
        enforceAccessPermission();
        return new Network[0];
    }

    public boolean isActiveNetworkMetered() {
        enforceAccessPermission();
        return true;
    }

    public int getRestrictBackgroundStatus() {
        return RESTRICT_BACKGROUND_STATUS_DISABLED;
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback) {
        register(networkCallback);
    }

    public void registerDefaultNetworkCallback(NetworkCallback networkCallback, Handler handler) {
        register(networkCallback);
    }

    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback) {
        register(networkCallback);
    }

    public void registerNetworkCallback(NetworkRequest request, NetworkCallback networkCallback, Handler handler) {
        register(networkCallback);
    }

    public void unregisterNetworkCallback(NetworkCallback networkCallback) {
        synchronized (callbacks) {
            if (networkCallback == null || !callbacks.remove(networkCallback)) {
                throw new IllegalArgumentException("NetworkCallback was not registered");
            }
        }
    }

    /** As registerDefaultNetworkCallback's path: the permission, then the callback kept once. */
    private void register(NetworkCallback networkCallback) {
        if (networkCallback == null) {
            throw new IllegalArgumentException("null NetworkCallback");
        }
        enforceAccessPermission();
        synchronized (callbacks) {
            if (!callbacks.add(networkCallback)) {
                throw new IllegalArgumentException("NetworkCallback was already registered");
            }
        }
    }

    /** ConnectivityService's enforceAccessPermission. */
    private void enforceAccessPermission() {
        context.enforceCallingOrSelfPermission(ACCESS_NETWORK_STATE, "ConnectivityService");
    }
}
