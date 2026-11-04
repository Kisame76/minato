package io.github.kisame76.minato.test.wifistate;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The network's broadcasts and Wi-Fi's state as an app without a location permission sees them (ADR-0054 step d)), on
 * a device online over Wi-Fi: the sticky CONNECTIVITY_ACTION, WIFI_STATE_CHANGED_ACTION and
 * NETWORK_STATE_CHANGED_ACTION that the registering of a receiver answers and delivers, with their extras (a WifiInfo
 * by its class's name, as its values are the device's), and WifiManager's state and connection info, without the link
 * speed, frequency and signal, which iOS does not tell an app. It ends with "done".
 */
public class WifiStateActivity extends Activity {
    private static final String TAG = "wifi-state-test";

    private final Handler mHandler = new Handler(Looper.getMainLooper());

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** The intent's extras by name, sorted, with their values; a NetworkInfo as its type name and state. */
    @SuppressWarnings("deprecation")
    private static String extras(Intent intent) {
        Bundle extras = intent.getExtras();
        if (extras == null) return "none";
        List<String> names = new ArrayList<>(extras.keySet());
        Collections.sort(names);
        StringBuilder line = new StringBuilder();
        for (String name : names) {
            Object value = extras.get(name);
            if (value instanceof NetworkInfo) {
                NetworkInfo info = (NetworkInfo) value;
                value = info.getTypeName() + " " + info.getState() + (info.getDetailedState() == null ? ""
                        : info.getDetailedState().name().equals(info.getState().name()) ? ""
                        : " " + info.getDetailedState());
            } else if (value instanceof WifiInfo) {
                value = "a WifiInfo";
            }
            if (line.length() > 0) line.append(", ");
            line.append(name).append('=').append(value);
        }
        return line.toString();
    }

    private BroadcastReceiver receiver(String name) {
        return new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                log(name + " receiver: initial sticky " + isInitialStickyBroadcast() + ", " + extras(intent));
            }
        };
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Intent connectivity = registerReceiver(receiver("connectivity"),
                new IntentFilter(ConnectivityManager.CONNECTIVITY_ACTION), Context.RECEIVER_NOT_EXPORTED);
        log("connectivity sticky: " + (connectivity == null ? "none" : extras(connectivity)));
        Intent wifiState = registerReceiver(receiver("wifi state"),
                new IntentFilter(WifiManager.WIFI_STATE_CHANGED_ACTION), Context.RECEIVER_NOT_EXPORTED);
        log("wifi state sticky: " + (wifiState == null ? "none" : extras(wifiState)));
        Intent network = registerReceiver(receiver("wifi network"),
                new IntentFilter(WifiManager.NETWORK_STATE_CHANGED_ACTION), Context.RECEIVER_NOT_EXPORTED);
        log("wifi network sticky: " + (network == null ? "none" : extras(network)));

        WifiManager wifi = getSystemService(WifiManager.class);
        log("wifi enabled " + wifi.isWifiEnabled() + ", state " + wifi.getWifiState());
        WifiInfo info = wifi.getConnectionInfo();
        log("connection info: ssid " + info.getSSID() + ", bssid " + info.getBSSID() + ", mac " + info.getMacAddress()
                + ", network id " + info.getNetworkId() + ", hidden " + info.getHiddenSSID() + ", supplicant "
                + info.getSupplicantState() + ", an address " + (info.getIpAddress() != 0));
        mHandler.postDelayed(() -> log("done"), 2000);
    }
}
