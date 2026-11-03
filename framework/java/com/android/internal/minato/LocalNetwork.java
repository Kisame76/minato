package com.android.internal.minato;

import android.app.ActivityThread;
import android.app.LocalNetworkBroadcasts;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.HandlerThread;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.SocketException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The host's network for ConnectivityManager (ADR-0054 step d)): what ConnectivityService tells an app, kept in the
 * app's process from the host's path monitor (APKRT_NR_NETWORK's state at the first use, APKRT_EVENT_NETWORK's on each
 * change). A path that can reach the network is one network under a netId of its own, from 100 on as NetIdManager
 * gives them, over Wi-Fi, cellular or Ethernet as the path's interface is (Wi-Fi when it names none of them),
 * validated, and metered when the path is expensive. Another transport, or a path back after none, is a new
 * network. The callbacks of every ConnectivityManager, which AOSP keeps in a static map too, hear of it as Android
 * 15's do: at the registering while a network is there and when one comes, onAvailable, onCapabilitiesChanged,
 * onLinkPropertiesChanged and onBlockedStatusChanged(false); onCapabilitiesChanged when only its metering changes;
 * onLost when it goes. They run on the Handler given at the registering, else on the process's ConnectivityThread,
 * and only while the callback is still registered, as AOSP's CallbackHandler drops a message whose callback is gone.
 * The sticky CONNECTIVITY_ACTION (ConnectivityService's, with its legacy extras; noConnectivity once the network is
 * gone) and WIFI_STATE_CHANGED_ACTION (Wi-Fi enabled while the path goes over it, else disabled) are made when a
 * receiver first asks for them and sent again on each change; WifiManager answers from the same state.
 */
@SuppressWarnings("deprecation") // NetworkInfo and CONNECTIVITY_ACTION, Android's legacy answers
public final class LocalNetwork {
    // guest_interface.h's APKRT_NETWORK_*.
    private static final int SATISFIED = 0x1;
    private static final int WIFI = 0x2;
    private static final int CELLULAR = 0x4;
    private static final int WIRED = 0x8;
    private static final int EXPENSIVE = 0x10;
    private static final int TRANSPORTS = WIFI | CELLULAR | WIRED;
    /** NetworkCapabilities' system API NET_CAPABILITY_NOT_VCN_MANAGED, which every network not of a VCN has. */
    private static final int NET_CAPABILITY_NOT_VCN_MANAGED = 28;
    /** NetIdManager's first netId. */
    private static final int FIRST_NET_ID = 100;

    private static final Object sLock = new Object();
    private static final Map<ConnectivityManager.NetworkCallback, Handler> sCallbacks = new LinkedHashMap<>();
    private static boolean sRead;
    private static int sState;
    private static Network sNetwork;
    private static int sNextNetId = FIRST_NET_ID;
    private static Handler sConnectivityThread;
    private static boolean sStickiesSent;

    private LocalNetwork() {}

    /** The state, read from the runtime at the first use: no network without one (-ENODATA, no host monitor). */
    private static void readLocked() {
        if (sRead) return;
        sRead = true;
        long state = ActivityThread.network();
        sState = state < 0 ? 0 : (int) state;
        if ((sState & SATISFIED) != 0) sNetwork = new Network(sNextNetId++);
    }

    /** The active network, or null. */
    public static Network active() {
        synchronized (sLock) {
            readLocked();
            return sNetwork;
        }
    }

    /** Whether the active network is metered: true without one, as ConnectivityService answers. */
    public static boolean metered() {
        synchronized (sLock) {
            readLocked();
            return sNetwork == null || (sState & EXPENSIVE) != 0;
        }
    }

    /** The legacy info of `network` if it is the active one (null: the active one), else null. */
    public static NetworkInfo info(Network network) {
        synchronized (sLock) {
            readLocked();
            if (sNetwork == null || (network != null && !network.equals(sNetwork))) return null;
            return infoLocked(sState, NetworkInfo.DetailedState.CONNECTED);
        }
    }

    /** The legacy info of a network of `state` in `detailed`, with ConnectivityService's empty extra info. */
    private static NetworkInfo infoLocked(int state, NetworkInfo.DetailedState detailed) {
        NetworkInfo info;
        if ((state & CELLULAR) != 0) {
            info = new NetworkInfo(ConnectivityManager.TYPE_MOBILE, 0, "MOBILE", "");
        } else if ((state & WIRED) != 0 && (state & WIFI) == 0) {
            info = new NetworkInfo(ConnectivityManager.TYPE_ETHERNET, 0, "ETHERNET", "");
        } else {
            info = new NetworkInfo(ConnectivityManager.TYPE_WIFI, 0, "WIFI", "");
        }
        info.setDetailedState(detailed, null, "");
        info.setIsAvailable(true);
        return info;
    }

    /** Whether Wi-Fi is enabled: while the path goes over it. */
    public static boolean wifi() {
        synchronized (sLock) {
            readLocked();
            return wifiLocked();
        }
    }

    private static boolean wifiLocked() {
        return sNetwork != null && (sState & WIFI) != 0;
    }

    /** The Wi-Fi connection: completed at wlan0's IPv4 address while the path goes over Wi-Fi, else none. */
    public static WifiInfo wifiInfo() {
        return wifi() ? WifiInfo.connected(wlan0Address()) : new WifiInfo();
    }

    /** wlan0's IPv4 address as AOSP's int, its first byte lowest; 0 for none. */
    private static int wlan0Address() {
        try {
            NetworkInterface wlan0 = NetworkInterface.getByName("wlan0");
            if (wlan0 == null) return 0;
            for (InetAddress address : Collections.list(wlan0.getInetAddresses())) {
                if (address instanceof Inet4Address) {
                    byte[] bytes = address.getAddress();
                    return (bytes[0] & 0xff) | (bytes[1] & 0xff) << 8 | (bytes[2] & 0xff) << 16
                            | (bytes[3] & 0xff) << 24;
                }
            }
        } catch (SocketException e) {
            // no interfaces to read: no address
        }
        return 0;
    }

    /** LocalActivityManagerService's: the sticky broadcasts before a filter looks for them, made once. */
    public static void prepareSticky() {
        List<Intent> stickies = new ArrayList<>();
        synchronized (sLock) {
            readLocked();
            if (sStickiesSent) return;
            sStickiesSent = true;
            if (sNetwork != null) stickies.add(connectivityLocked(sState, false));
            stickies.add(wifiStateLocked(wifiLocked(), null));
        }
        send(stickies);
    }

    /** ConnectivityService's CONNECTIVITY_ACTION for a network of `state`, connected or, `gone`, lost. */
    private static Intent connectivityLocked(int state, boolean gone) {
        NetworkInfo info = infoLocked(state, gone ? NetworkInfo.DetailedState.DISCONNECTED
                : NetworkInfo.DetailedState.CONNECTED);
        Intent intent = new Intent(ConnectivityManager.CONNECTIVITY_ACTION);
        intent.putExtra(ConnectivityManager.EXTRA_NETWORK_INFO, info);
        intent.putExtra(ConnectivityManager.EXTRA_NETWORK_TYPE, info.getType());
        intent.putExtra(ConnectivityManager.EXTRA_EXTRA_INFO, info.getExtraInfo());
        intent.putExtra("inetCondition", 0);  // ConnectivityManager's hidden EXTRA_INET_CONDITION
        if (gone) intent.putExtra(ConnectivityManager.EXTRA_NO_CONNECTIVITY, true);
        return intent;
    }

    /** WifiService's WIFI_STATE_CHANGED_ACTION, from `previous` (null: the start, ENABLING or UNKNOWN). */
    private static Intent wifiStateLocked(boolean enabled, Boolean previous) {
        Intent intent = new Intent(WifiManager.WIFI_STATE_CHANGED_ACTION);
        intent.putExtra(WifiManager.EXTRA_WIFI_STATE,
                enabled ? WifiManager.WIFI_STATE_ENABLED : WifiManager.WIFI_STATE_DISABLED);
        int before = previous == null ? (enabled ? WifiManager.WIFI_STATE_ENABLING : WifiManager.WIFI_STATE_UNKNOWN)
                : previous ? WifiManager.WIFI_STATE_ENABLED : WifiManager.WIFI_STATE_DISABLED;
        intent.putExtra(WifiManager.EXTRA_PREVIOUS_WIFI_STATE, before);
        return intent;
    }

    private static void send(List<Intent> stickies) {
        for (Intent intent : stickies) LocalNetworkBroadcasts.sendSticky(intent);
    }

    /** The legacy info of the active network if it is of `type`, else null. */
    public static NetworkInfo info(int type) {
        NetworkInfo info = info(null);
        return info != null && info.getType() == type ? info : null;
    }

    /** The capabilities of `network` if it is the active one, else null. */
    public static NetworkCapabilities capabilities(Network network) {
        synchronized (sLock) {
            readLocked();
            return network != null && network.equals(sNetwork) ? capabilitiesLocked() : null;
        }
    }

    private static NetworkCapabilities capabilitiesLocked() {
        NetworkCapabilities capabilities = new NetworkCapabilities();
        if ((sState & CELLULAR) != 0) {
            capabilities.addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR);
        } else if ((sState & WIRED) != 0 && (sState & WIFI) == 0) {
            capabilities.addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET);
        } else {
            capabilities.addTransportType(NetworkCapabilities.TRANSPORT_WIFI);
        }
        capabilities.addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_CONGESTED)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED)
                .addCapability(NET_CAPABILITY_NOT_VCN_MANAGED);
        if ((sState & EXPENSIVE) == 0) capabilities.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        return capabilities;
    }

    /** The link of `network` if it is the active one, else null: the guest's name of its interface. */
    public static LinkProperties link(Network network) {
        synchronized (sLock) {
            readLocked();
            return network != null && network.equals(sNetwork) ? linkLocked() : null;
        }
    }

    private static LinkProperties linkLocked() {
        LinkProperties link = new LinkProperties();
        if ((sState & CELLULAR) != 0) {
            link.setInterfaceName("rmnet_data0");
        } else if ((sState & WIRED) != 0 && (sState & WIFI) == 0) {
            link.setInterfaceName("eth0");
        } else {
            link.setInterfaceName("wlan0");
        }
        return link;
    }

    /** The networks: the active one, or none. */
    public static Network[] all() {
        Network network = active();
        return network == null ? new Network[0] : new Network[] {network};
    }

    /**
     * Keeps `callback` until its unregistering, on `handler` (null: ConnectivityThread's); it hears of the active
     * network at once. False when it is registered already.
     */
    public static boolean register(ConnectivityManager.NetworkCallback callback, Handler handler) {
        synchronized (sLock) {
            readLocked();
            if (sCallbacks.containsKey(callback)) return false;
            Handler target = handler != null ? handler : connectivityThreadLocked();
            sCallbacks.put(callback, target);
            if (sNetwork != null) available(callback, target, sNetwork, capabilitiesLocked(), linkLocked());
            return true;
        }
    }

    /** Forgets `callback`; false when it is not registered. */
    public static boolean unregister(ConnectivityManager.NetworkCallback callback) {
        synchronized (sLock) {
            return sCallbacks.remove(callback) != null;
        }
    }

    /** APKRT_EVENT_NETWORK: the host's path changed to `state`. */
    public static void onEvent(int state) {
        List<Intent> stickies = new ArrayList<>();
        boolean wifiBefore;
        synchronized (sLock) {
            readLocked();
            wifiBefore = wifiLocked();
            changeLocked(state, stickies);
            if (sStickiesSent && wifiLocked() != wifiBefore) stickies.add(wifiStateLocked(wifiLocked(), wifiBefore));
            if (!sStickiesSent) stickies.clear();
        }
        send(stickies);
    }

    /** The change to `state`: the callbacks posted, and CONNECTIVITY_ACTION for a network gone or come. */
    private static void changeLocked(int state, List<Intent> stickies) {
        int before = sState;
        Network old = sNetwork;
        sState = state;
        boolean up = (state & SATISFIED) != 0;
        if (old != null && up && (before & TRANSPORTS) == (state & TRANSPORTS)) {
            if (((before ^ state) & EXPENSIVE) != 0) {
                NetworkCapabilities capabilities = capabilitiesLocked();
                for (Map.Entry<ConnectivityManager.NetworkCallback, Handler> entry : entries()) {
                    entry.getValue().post(() -> {
                        if (registered(entry.getKey())) {
                            entry.getKey().onCapabilitiesChanged(old, new NetworkCapabilities(capabilities));
                        }
                    });
                }
            }
            return;
        }
        if (old != null) {
            sNetwork = null;
            if (!up) stickies.add(connectivityLocked(before, true));
            for (Map.Entry<ConnectivityManager.NetworkCallback, Handler> entry : entries()) {
                entry.getValue().post(() -> {
                    if (registered(entry.getKey())) entry.getKey().onLost(old);
                });
            }
        }
        if (up) {
            sNetwork = new Network(sNextNetId++);
            stickies.add(connectivityLocked(state, false));
            for (Map.Entry<ConnectivityManager.NetworkCallback, Handler> entry : entries()) {
                available(entry.getKey(), entry.getValue(), sNetwork, capabilitiesLocked(), linkLocked());
            }
        }
    }

    private static boolean registered(ConnectivityManager.NetworkCallback callback) {
        synchronized (sLock) {
            return sCallbacks.containsKey(callback);
        }
    }

    private static List<Map.Entry<ConnectivityManager.NetworkCallback, Handler>> entries() {
        return new ArrayList<>(sCallbacks.entrySet());
    }

    /** As NetworkCallback's hidden onAvailable(Network, NetworkCapabilities, LinkProperties, int) calls them. */
    private static void available(ConnectivityManager.NetworkCallback callback, Handler handler, Network network,
            NetworkCapabilities capabilities, LinkProperties link) {
        handler.post(() -> {
            if (!registered(callback)) return;
            callback.onAvailable(network);
            callback.onCapabilitiesChanged(network, capabilities);
            callback.onLinkPropertiesChanged(network, link);
            callback.onBlockedStatusChanged(network, false);
        });
    }

    private static Handler connectivityThreadLocked() {
        if (sConnectivityThread == null) {
            HandlerThread thread = new HandlerThread("ConnectivityThread");
            thread.start();
            sConnectivityThread = new Handler(thread.getLooper());
        }
        return sConnectivityThread;
    }
}
