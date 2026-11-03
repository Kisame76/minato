package io.github.kisame76.minato.test.network;

import android.app.Activity;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * The device's network as an app sees it (ADR-0054 step d)): the active network, its NetworkInfo and capabilities, and
 * the default network callback's first calls, each kind once, in the order they come, within 2 s of registering it on
 * the main Looper. The device is online over Wi-Fi or Ethernet; lines that would name which say "wifi or ethernet".
 * It ends with "done".
 */
public class NetworkActivity extends Activity {
    private static final String TAG = "network-test";
    private static final int[] CAPABILITIES = {
        NetworkCapabilities.NET_CAPABILITY_INTERNET, NetworkCapabilities.NET_CAPABILITY_VALIDATED,
        NetworkCapabilities.NET_CAPABILITY_NOT_METERED, NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED,
        NetworkCapabilities.NET_CAPABILITY_TRUSTED, NetworkCapabilities.NET_CAPABILITY_NOT_VPN,
        NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING, NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED,
    };
    private static final String[] CAPABILITY_NAMES = {
        "INTERNET", "VALIDATED", "NOT_METERED", "NOT_RESTRICTED", "TRUSTED", "NOT_VPN", "NOT_ROAMING", "NOT_SUSPENDED",
    };

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final List<String> mSeen = new ArrayList<>();

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** A callback's call, logged the first time its kind comes. */
    private void call(String kind, String line) {
        if (mSeen.contains(kind)) return;
        mSeen.add(kind);
        log("callback: " + line);
    }

    private static String wifiOrEthernet(boolean either, String otherwise) {
        return either ? "wifi or ethernet" : otherwise;
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ConnectivityManager connectivity = getSystemService(ConnectivityManager.class);
        Network active = connectivity.getActiveNetwork();
        log("active network: " + (active != null));
        NetworkInfo info = connectivity.getActiveNetworkInfo();
        log("network info: " + (info == null ? "none" : wifiOrEthernet(info.getType() == ConnectivityManager.TYPE_WIFI
                || info.getType() == ConnectivityManager.TYPE_ETHERNET, info.getTypeName()) + ", " + info.getState()
                + ", " + info.getDetailedState() + ", connected " + info.isConnected() + ", available "
                + info.isAvailable() + ", roaming " + info.isRoaming()));
        log("metered: " + connectivity.isActiveNetworkMetered());
        NetworkCapabilities capabilities = active == null ? null : connectivity.getNetworkCapabilities(active);
        if (capabilities == null) {
            log("capabilities: none");
        } else {
            StringBuilder line = new StringBuilder(wifiOrEthernet(
                    capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                    || capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET), "another transport"));
            for (int i = 0; i < CAPABILITIES.length; i++) {
                line.append(", ").append(CAPABILITY_NAMES[i]).append(' ')
                        .append(capabilities.hasCapability(CAPABILITIES[i]));
            }
            log("capabilities: " + line);
        }
        LinkProperties link = active == null ? null : connectivity.getLinkProperties(active);
        log("link properties: " + (link != null));
        log("all networks: " + (connectivity.getAllNetworks().length > 0 ? "some" : "none"));

        ConnectivityManager.NetworkCallback callback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                call("available", "onAvailable, the active network " + network.equals(active));
            }

            @Override
            public void onCapabilitiesChanged(Network network, NetworkCapabilities changed) {
                call("capabilities", "onCapabilitiesChanged, INTERNET "
                        + changed.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET));
            }

            @Override
            public void onLinkPropertiesChanged(Network network, LinkProperties changed) {
                call("link", "onLinkPropertiesChanged");
            }

            @Override
            public void onBlockedStatusChanged(Network network, boolean blocked) {
                call("blocked", "onBlockedStatusChanged " + blocked);
            }

            @Override
            public void onLost(Network network) {
                call("lost", "onLost");
            }
        };
        connectivity.registerDefaultNetworkCallback(callback, mHandler);
        mHandler.postDelayed(() -> {
            connectivity.unregisterNetworkCallback(callback);
            log("done");
        }, 2000);
    }
}
