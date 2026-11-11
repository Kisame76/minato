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
 * 15's do while it satisfies their NetworkRequest's capabilities (any network for the default network's): at the
 * registering while it is there and when it comes or starts to satisfy them, onAvailable, onCapabilitiesChanged,
 * onLinkPropertiesChanged and onBlockedStatusChanged(false); onCapabilitiesChanged when only its metering changes and
 * it still does; onLost when it goes or no longer does. A request with a timeout that no network satisfies when it
 * runs out hears onUnavailable and nothing after it, as ConnectivityService releases it, but stays registered for its
 * unregistering. They run on the Handler given at the registering, else on the process's ConnectivityThread, and
 * only while the callback is still registered, as AOSP's CallbackHandler drops a message whose callback is gone.
 * The sticky CONNECTIVITY_ACTION (ConnectivityService's, with its legacy extras; noConnectivity once the network is
 * gone), WIFI_STATE_CHANGED_ACTION (Wi-Fi enabled while the path goes over it, else disabled) and Wi-Fi's
 * NETWORK_STATE_CHANGED_ACTION (its NetworkInfo connected or disconnected, without the BSSID and WifiInfo, which need
 * a location permission) are made when a receiver first asks for them and sent again on each change; WifiManager
 * answers from the same state. Data Saver is the host's Low Data Mode (a constrained path): the restrict-background
 * status is ENABLED while the network's path is constrained, else DISABLED, and ACTION_RESTRICT_BACKGROUND_CHANGED
 * goes to registered receivers on each change, as NetworkPolicyManagerService sends it.
 */
@SuppressWarnings("deprecation") // NetworkInfo and CONNECTIVITY_ACTION, Android's legacy answers
public final class LocalNetwork {
    // guest_interface.h's APKRT_NETWORK_*.
    private static final int SATISFIED = 0x1;
    private static final int WIFI = 0x2;
    private static final int CELLULAR = 0x4;
    private static final int WIRED = 0x8;
    private static final int EXPENSIVE = 0x10;
    private static final int CONSTRAINED = 0x20;
    private static final int RADIO_SHIFT = 8;
    private static final int RADIO_MASK = 0xff00;
    /** TelephonyManager's getNetworkTypeName for NETWORK_TYPE_UNKNOWN (0) to NETWORK_TYPE_NR (20). */
    private static final String[] RADIO_NAMES = {
        "UNKNOWN", "GPRS", "EDGE", "UMTS", "CDMA", "CDMA - EvDo rev. 0", "CDMA - EvDo rev. A", "CDMA - 1xRTT", "HSDPA",
        "HSUPA", "HSPA", "iDEN", "CDMA - EvDo rev. B", "LTE", "CDMA - eHRPD", "HSPA+", "GSM", "TD_SCDMA", "IWLAN",
        "LTE_CA", "NR",
    };
    private static final int TRANSPORTS = WIFI | CELLULAR | WIRED;
    /** NetworkCapabilities' system API NET_CAPABILITY_NOT_VCN_MANAGED, which every network not of a VCN has. */
    private static final int NET_CAPABILITY_NOT_VCN_MANAGED = 28;
    /** NetIdManager's first netId. */
    private static final int FIRST_NET_ID = 100;

    private static final Object sLock = new Object();
    private static final Map<ConnectivityManager.NetworkCallback, Listener> sCallbacks = new LinkedHashMap<>();
    private static boolean sRead;
    private static int sState;
    private static Network sNetwork;
    private static int sNextNetId = FIRST_NET_ID;
    private static Handler sConnectivityThread;
    private static boolean sStickiesSent;

    private LocalNetwork() {}

    /** A callback's Handler and the capabilities it asks for (null: any network's, the default network's). */
    private static final class Listener {
        final Handler handler;
        final NetworkCapabilities need;
        boolean released;  // a request whose timeout ran out: ConnectivityService has let it go

        Listener(Handler handler, NetworkCapabilities need) {
            this.handler = handler;
            this.need = need;
        }

        /** Whether a network of `capabilities` (null: none) is one this callback hears of. */
        boolean wants(NetworkCapabilities capabilities) {
            return !released && capabilities != null
                    && (need == null || need.satisfiedByNetworkCapabilities(capabilities));
        }
    }

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

    /** ConnectivityManager's restrict-background status: ENABLED in Low Data Mode, else DISABLED. */
    public static int restrictBackgroundStatus() {
        synchronized (sLock) {
            readLocked();
            return restrictedLocked() ? ConnectivityManager.RESTRICT_BACKGROUND_STATUS_ENABLED
                    : ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED;
        }
    }

    private static boolean restrictedLocked() {
        return sNetwork != null && (sState & CONSTRAINED) != 0;
    }

    /** The legacy info of `network` if it is the active one (null: the active one), else null. */
    public static NetworkInfo info(Network network) {
        synchronized (sLock) {
            readLocked();
            if (sNetwork == null || (network != null && !network.equals(sNetwork))) return null;
            return infoLocked(sState, NetworkInfo.DetailedState.CONNECTED);
        }
    }

    /**
     * The legacy info of a network of `state` in `detailed`, with ConnectivityService's empty extra info; a cellular
     * one has its radio as subtype, named as TelephonyManager names it.
     */
    private static NetworkInfo infoLocked(int state, NetworkInfo.DetailedState detailed) {
        NetworkInfo info;
        if ((state & CELLULAR) != 0) {
            int radio = (state & RADIO_MASK) >> RADIO_SHIFT;
            info = new NetworkInfo(ConnectivityManager.TYPE_MOBILE, radio, "MOBILE",
                    radio < RADIO_NAMES.length ? RADIO_NAMES[radio] : "UNKNOWN(" + radio + ")");
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
            stickies.add(wifiNetworkLocked(wifiLocked()));
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

    /** ClientModeImpl's NETWORK_STATE_CHANGED_ACTION for an app without a location permission: the NetworkInfo. */
    private static Intent wifiNetworkLocked(boolean connected) {
        NetworkInfo info = new NetworkInfo(ConnectivityManager.TYPE_WIFI, 0, "WIFI", "");
        info.setDetailedState(connected ? NetworkInfo.DetailedState.CONNECTED : NetworkInfo.DetailedState.DISCONNECTED,
                null, "");
        info.setIsAvailable(connected);
        Intent intent = new Intent(WifiManager.NETWORK_STATE_CHANGED_ACTION);
        intent.putExtra(WifiManager.EXTRA_NETWORK_INFO, info);
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
        return capabilitiesOf(sState);
    }

    /** The capabilities of a network of `state`. */
    private static NetworkCapabilities capabilitiesOf(int state) {
        NetworkCapabilities capabilities = new NetworkCapabilities();
        if ((state & CELLULAR) != 0) {
            capabilities.addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR);
        } else if ((state & WIRED) != 0 && (state & WIFI) == 0) {
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
        if ((state & EXPENSIVE) == 0) capabilities.addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        return capabilities;
    }

    /**
     * The link of `network` if it is the active one, else null: the runtime's snapshot of the host's network
     * (APKRT_NETWORK_LINK: interface, addresses, DNS servers, routes, MTU, the same data the guest's getifaddrs and
     * netlink answer from), or, where the runtime gives none, the guest's name of the interface alone.
     */
    public static LinkProperties link(Network network) {
        synchronized (sLock) {
            readLocked();
            return network != null && network.equals(sNetwork) ? linkLocked() : null;
        }
    }

    private static LinkProperties linkLocked() {
        LinkProperties snapshot = snapshot();
        if (snapshot != null) {
            return snapshot;
        }
        LinkProperties link = new LinkProperties();
        link.setInterfaceName(transportName());
        return link;
    }

    /** The guest's name of the interface by the transport the host reports (the runtime's netlink names them so). */
    private static String transportName() {
        if ((sState & CELLULAR) != 0) {
            return "rmnet_data0";
        } else if ((sState & WIRED) != 0 && (sState & WIFI) == 0) {
            return "eth0";
        }
        return "wlan0";
    }

    /**
     * The runtime's snapshot (APKRT_NETWORK_LINK, version 1, little-endian: version, flags, the interface name, the
     * MTU, the addresses with their prefix lengths, the DNS servers and the routes with their gateways, each with its
     * family 4 or 6), or null when the runtime gives none or its bytes are not a snapshot this code reads.
     */
    private static LinkProperties snapshot() {
        byte[] buffer = new byte[512];
        int size = ActivityThread.networkLink(buffer);
        if (size > buffer.length) {
            buffer = new byte[size];
            size = ActivityThread.networkLink(buffer);
        }
        if (size <= 0 || size > buffer.length) {
            return null;
        }
        try {
            java.nio.ByteBuffer in = java.nio.ByteBuffer.wrap(buffer, 0, size).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            if (in.get() != 1) {
                return null;
            }
            in.get();  // flags: which parts the sandbox let the runtime read; an empty list says the rest
            LinkProperties link = new LinkProperties();
            byte[] name = new byte[in.get() & 0xff];
            in.get(name);
            // A network whose interface the runtime cannot read comes as an empty snapshot (name length 0, no lists):
            // the link is still there, named by the transport as before.
            String snapshotName = new String(name, java.nio.charset.StandardCharsets.UTF_8);
            link.setInterfaceName(snapshotName.isEmpty() ? transportName() : snapshotName);
            link.setMtu(in.getInt());
            for (int count = in.get() & 0xff; count > 0; count--) {
                int family = in.get();
                int prefix = in.get() & 0xff;
                link.addLinkAddress(new android.net.LinkAddress(address(in, family), prefix));
            }
            for (int count = in.get() & 0xff; count > 0; count--) {
                link.addDnsServer(address(in, in.get()));
            }
            for (int count = in.get() & 0xff; count > 0; count--) {
                int family = in.get();
                int prefix = in.get() & 0xff;
                java.net.InetAddress destination = address(in, family);
                java.net.InetAddress gateway = in.get() != 0 ? address(in, family) : null;
                link.addRoute(new android.net.RouteInfo(new android.net.IpPrefix(destination, prefix), gateway,
                        link.getInterfaceName()));
            }
            return link;
        } catch (java.nio.BufferUnderflowException | java.net.UnknownHostException | IllegalArgumentException e) {
            return null;
        }
    }

    private static java.net.InetAddress address(java.nio.ByteBuffer in, int family)
            throws java.net.UnknownHostException {
        byte[] bytes = new byte[family == 6 ? 16 : 4];
        in.get(bytes);
        return java.net.InetAddress.getByAddress(bytes);
    }

    /** The networks: the active one, or none. */
    public static Network[] all() {
        Network network = active();
        return network == null ? new Network[0] : new Network[] {network};
    }

    /**
     * Keeps `callback` until its unregistering, on `handler` (null: ConnectivityThread's), for networks that satisfy
     * `need` (null: any); it hears of the active network at once if that one does. With `timeoutMs` above 0, a
     * request's: onUnavailable then, unless a network satisfies it. False when it is registered already.
     */
    public static boolean register(ConnectivityManager.NetworkCallback callback, Handler handler,
            NetworkCapabilities need, int timeoutMs) {
        synchronized (sLock) {
            readLocked();
            if (sCallbacks.containsKey(callback)) return false;
            Listener listener = new Listener(handler != null ? handler : connectivityThreadLocked(),
                    need == null ? null : new NetworkCapabilities(need));
            sCallbacks.put(callback, listener);
            NetworkCapabilities capabilities = sNetwork == null ? null : capabilitiesLocked();
            if (listener.wants(capabilities)) {
                available(callback, listener.handler, sNetwork, capabilities, linkLocked());
            }
            if (timeoutMs > 0) listener.handler.postDelayed(() -> timedOut(callback, listener), timeoutMs);
            return true;
        }
    }

    /** ConnectivityService's timeout of a request: released, with onUnavailable, unless a network satisfies it. */
    private static void timedOut(ConnectivityManager.NetworkCallback callback, Listener listener) {
        synchronized (sLock) {
            if (sCallbacks.get(callback) != listener) return;
            if (listener.wants(sNetwork == null ? null : capabilitiesLocked())) return;
            listener.released = true;
        }
        callback.onUnavailable();
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
        boolean wifiBefore, restrictedBefore, restricted;
        synchronized (sLock) {
            readLocked();
            wifiBefore = wifiLocked();
            restrictedBefore = restrictedLocked();
            changeLocked(state, stickies);
            if (sStickiesSent && wifiLocked() != wifiBefore) {
                stickies.add(wifiStateLocked(wifiLocked(), wifiBefore));
                stickies.add(wifiNetworkLocked(wifiLocked()));
            }
            if (!sStickiesSent) stickies.clear();
            restricted = restrictedLocked();
        }
        send(stickies);
        if (restricted != restrictedBefore) {
            Intent intent = new Intent(ConnectivityManager.ACTION_RESTRICT_BACKGROUND_CHANGED);
            intent.setFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
            LocalNetworkBroadcasts.send(intent);
        }
    }

    /** The change to `state`: the callbacks posted, and CONNECTIVITY_ACTION for a network gone or come. */
    private static void changeLocked(int state, List<Intent> stickies) {
        int before = sState;
        Network old = sNetwork;
        NetworkCapabilities was = old == null ? null : capabilitiesOf(before);
        sState = state;
        boolean up = (state & SATISFIED) != 0;
        if (old != null && up && (before & TRANSPORTS) == (state & TRANSPORTS)) {
            if (((before ^ state) & EXPENSIVE) != 0) {
                NetworkCapabilities capabilities = capabilitiesLocked();
                for (Map.Entry<ConnectivityManager.NetworkCallback, Listener> entry : entries()) {
                    ConnectivityManager.NetworkCallback callback = entry.getKey();
                    Listener listener = entry.getValue();
                    boolean wanted = listener.wants(was), wants = listener.wants(capabilities);
                    if (wanted && wants) {
                        listener.handler.post(() -> {
                            if (registered(callback)) {
                                callback.onCapabilitiesChanged(old, new NetworkCapabilities(capabilities));
                            }
                        });
                    } else if (wanted) {
                        lost(callback, listener, old);
                    } else if (wants) {
                        available(callback, listener.handler, old, capabilities, linkLocked());
                    }
                }
            }
            return;
        }
        if (old != null) {
            sNetwork = null;
            if (!up) stickies.add(connectivityLocked(before, true));
            for (Map.Entry<ConnectivityManager.NetworkCallback, Listener> entry : entries()) {
                if (entry.getValue().wants(was)) lost(entry.getKey(), entry.getValue(), old);
            }
        }
        if (up) {
            sNetwork = new Network(sNextNetId++);
            stickies.add(connectivityLocked(state, false));
            NetworkCapabilities capabilities = capabilitiesLocked();
            for (Map.Entry<ConnectivityManager.NetworkCallback, Listener> entry : entries()) {
                if (entry.getValue().wants(capabilities)) {
                    available(entry.getKey(), entry.getValue().handler, sNetwork, capabilities, linkLocked());
                }
            }
        }
    }

    private static void lost(ConnectivityManager.NetworkCallback callback, Listener listener, Network network) {
        listener.handler.post(() -> {
            if (registered(callback)) callback.onLost(network);
        });
    }

    private static boolean registered(ConnectivityManager.NetworkCallback callback) {
        synchronized (sLock) {
            return sCallbacks.containsKey(callback);
        }
    }

    private static List<Map.Entry<ConnectivityManager.NetworkCallback, Listener>> entries() {
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
