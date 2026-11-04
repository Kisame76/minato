package io.github.kisame76.minato.test.networkrequest;

import android.app.Activity;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * NetworkRequest and ConnectivityManager's requests as an app sees them (ADR-0054 step d)), on a device online over
 * Wi-Fi or Ethernet with no cellular network: built requests and their capabilities, which capabilities satisfy them,
 * then one after another on the main Looper, each for 1.5 s: a listen for cellular, a listen and a request for the
 * Internet, a request for cellular with a timeout of 1 s (for 2.5 s), each callback's kinds of calls in the order
 * they first come, and a request with a timeout of 0. It ends with "done".
 */
public class RequestActivity extends Activity {
    private static final String TAG = "network-request-test";

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private ConnectivityManager mConnectivity;
    private NetworkRequest mInternet;
    private NetworkRequest mCellular;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** A callback that keeps the kinds of its calls in the order they first come. */
    private static final class Recorder extends ConnectivityManager.NetworkCallback {
        final List<String> kinds = new ArrayList<>();

        private void seen(String kind) {
            if (!kinds.contains(kind)) kinds.add(kind);
        }

        @Override
        public void onAvailable(Network network) {
            seen("onAvailable");
        }

        @Override
        public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) {
            seen("onCapabilitiesChanged");
        }

        @Override
        public void onLinkPropertiesChanged(Network network, LinkProperties link) {
            seen("onLinkPropertiesChanged");
        }

        @Override
        public void onBlockedStatusChanged(Network network, boolean blocked) {
            seen("onBlockedStatusChanged " + blocked);
        }

        @Override
        public void onLost(Network network) {
            seen("onLost");
        }

        @Override
        public void onUnavailable() {
            seen("onUnavailable");
        }

        String calls() {
            return kinds.isEmpty() ? "none" : String.join(", ", kinds);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mConnectivity = getSystemService(ConnectivityManager.class);
        NetworkRequest plain = new NetworkRequest.Builder().build();
        log("default request: capabilities " + Arrays.toString(plain.getCapabilities()) + ", transports "
                + Arrays.toString(plain.getTransportTypes()));
        mInternet = new NetworkRequest.Builder().addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build();
        log("internet request: capabilities " + Arrays.toString(mInternet.getCapabilities()) + ", INTERNET "
                + mInternet.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) + ", wifi "
                + mInternet.hasTransport(NetworkCapabilities.TRANSPORT_WIFI));
        mCellular = new NetworkRequest.Builder().addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR).build();
        log("cellular request: transports " + Arrays.toString(mCellular.getTransportTypes()) + ", cellular "
                + mCellular.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR));
        NetworkRequest mms = new NetworkRequest.Builder().addCapability(NetworkCapabilities.NET_CAPABILITY_MMS).build();
        log("mms request: capabilities " + Arrays.toString(mms.getCapabilities()));
        NetworkRequest removed = new NetworkRequest.Builder()
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN).build();
        log("request without NOT_VPN: capabilities " + Arrays.toString(removed.getCapabilities()));
        log("equal requests: " + mInternet.equals(new NetworkRequest.Builder()
                .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build()) + ", internet and cellular "
                + mInternet.equals(mCellular));
        Network active = mConnectivity.getActiveNetwork();
        NetworkCapabilities capabilities = active == null ? null : mConnectivity.getNetworkCapabilities(active);
        log("satisfied by the active network: default " + plain.canBeSatisfiedBy(capabilities) + ", internet "
                + mInternet.canBeSatisfiedBy(capabilities) + ", cellular " + mCellular.canBeSatisfiedBy(capabilities)
                + ", mms " + mms.canBeSatisfiedBy(capabilities) + "; by null " + mInternet.canBeSatisfiedBy(null));
        listenCellular();
    }

    private void listenCellular() {
        Recorder recorder = new Recorder();
        mConnectivity.registerNetworkCallback(mCellular, recorder, mHandler);
        mHandler.postDelayed(() -> {
            mConnectivity.unregisterNetworkCallback(recorder);
            log("cellular listen: " + recorder.calls());
            listenInternet();
        }, 1500);
    }

    private void listenInternet() {
        Recorder recorder = new Recorder();
        mConnectivity.registerNetworkCallback(mInternet, recorder, mHandler);
        mHandler.postDelayed(() -> {
            mConnectivity.unregisterNetworkCallback(recorder);
            log("internet listen: " + recorder.calls());
            requestInternet();
        }, 1500);
    }

    private void requestInternet() {
        Recorder recorder = new Recorder();
        mConnectivity.requestNetwork(mInternet, recorder, mHandler);
        mHandler.postDelayed(() -> {
            mConnectivity.unregisterNetworkCallback(recorder);
            log("internet request: " + recorder.calls());
            requestCellular();
        }, 1500);
    }

    private void requestCellular() {
        Recorder recorder = new Recorder();
        mConnectivity.requestNetwork(mCellular, recorder, mHandler, 1000);
        mHandler.postDelayed(() -> {
            log("cellular request with a timeout of 1 s: " + recorder.calls());
            String unregistered;
            try {
                mConnectivity.unregisterNetworkCallback(recorder);
                unregistered = "unregistered";
            } catch (RuntimeException e) {
                unregistered = e.getClass().getSimpleName() + ": " + e.getMessage();
            }
            log("its unregistering after that: " + unregistered);
            requestWithoutTime();
        }, 2500);
    }

    private void requestWithoutTime() {
        Recorder recorder = new Recorder();
        try {
            mConnectivity.requestNetwork(mInternet, recorder, 0);
            log("request with a timeout of 0: taken");
            mConnectivity.unregisterNetworkCallback(recorder);
        } catch (RuntimeException e) {
            log("request with a timeout of 0: " + e.getClass().getSimpleName() + ": " + e.getMessage());
        }
        log("done");
    }
}
