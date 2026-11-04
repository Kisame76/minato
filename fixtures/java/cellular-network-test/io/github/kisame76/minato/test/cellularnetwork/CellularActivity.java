package io.github.kisame76.minato.test.cellularnetwork;

import android.app.Activity;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * A cellular network as an app sees it (ADR-0054 step d)), on a device online over its cellular network alone:
 * "ready", then at the default network callback's first capabilities the network's legacy info with its subtype, the
 * same from getActiveNetworkInfo, its capabilities and whether it is metered. It ends with "done" a second later.
 */
public class CellularActivity extends Activity {
    private static final String TAG = "cellular-network-test";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mSeen;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @SuppressWarnings("deprecation")
    private static String info(NetworkInfo info) {
        return info == null ? "none" : info.getTypeName() + " " + info.getType() + ", subtype " + info.getSubtype()
                + " " + info.getSubtypeName() + ", " + info.getState() + ", roaming " + info.isRoaming();
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ConnectivityManager connectivity = getSystemService(ConnectivityManager.class);
        connectivity.registerDefaultNetworkCallback(new ConnectivityManager.NetworkCallback() {
            @Override
            public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) {
                if (mSeen) return;
                mSeen = true;
                log("network info: " + info(connectivity.getNetworkInfo(network)));
                log("active network info: " + info(connectivity.getActiveNetworkInfo()));
                log("capabilities: cellular " + capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)
                        + ", wifi " + capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) + ", INTERNET "
                        + capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) + ", NOT_METERED "
                        + capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED));
                log("metered: " + connectivity.isActiveNetworkMetered());
                mHandler.postDelayed(() -> {
                    connectivity.unregisterNetworkCallback(this);
                    log("done");
                }, 1000);
            }
        }, mHandler);
        log("ready");
    }
}
