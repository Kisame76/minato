package io.github.kisame76.minato.test.netlink;

import android.app.Application;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.RouteInfo;
import android.util.Log;

import java.net.InetAddress;
import java.util.List;

/**
 * The link properties test: what getLinkProperties of the active network hands out, as structure: an interface name,
 * addresses with valid prefix lengths that are no wildcard, routes that belong to the interface and include the default
 * route, DNS servers that are no wildcard, lists nobody can change, and a toString that prints what the getters hold.
 * The values themselves are the network's own and are only shown (the -device tag).
 */
public class LinkApplication extends Application {
    private static String TAG = "net-link-test";

    @Override
    public void onCreate() {
        super.onCreate();
        TAG = getApplicationInfo().loadLabel(getPackageManager()).toString();
        new Thread(this::run).start();
    }

    private void run() {
        try {
            check();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
        }
        Log.i(TAG, "done");
    }

    private void check() {
        ConnectivityManager manager = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        Network active = manager.getActiveNetwork();
        Log.i(TAG, "active network: " + (active != null));
        LinkProperties link = active == null ? null : manager.getLinkProperties(active);
        Log.i(TAG, "link properties: " + (link != null));
        if (link == null) return;
        String device = TAG + "-device";
        String name = link.getInterfaceName();
        Log.i(device, "interface " + name + ", mtu " + link.getMtu());
        Log.i(device, "addresses " + link.getLinkAddresses());
        Log.i(device, "routes " + link.getRoutes());
        Log.i(device, "dns " + link.getDnsServers());
        Log.i(TAG, "interface named: " + (name != null && !name.isEmpty()));

        List<LinkAddress> addresses = link.getLinkAddresses();
        boolean prefixes = true;
        boolean wildcard = false;
        for (LinkAddress address : addresses) {
            int max = address.getAddress().getAddress().length * 8;
            prefixes &= address.getPrefixLength() >= 0 && address.getPrefixLength() <= max;
            wildcard |= address.getAddress().isAnyLocalAddress();
        }
        Log.i(TAG, "addresses: " + (addresses.isEmpty() ? "none" : "some") + ", prefixes valid " + prefixes
            + ", wildcard " + wildcard);

        List<RouteInfo> routes = link.getRoutes();
        boolean sameInterface = true;
        boolean defaultRoute = false;
        boolean gatewayWildcard = false;
        for (RouteInfo route : routes) {
            sameInterface &= name.equals(route.getInterface());
            if (route.isDefaultRoute()) {
                defaultRoute = true;
                InetAddress gateway = route.getGateway();
                gatewayWildcard |= gateway == null || gateway.isAnyLocalAddress();
            }
        }
        Log.i(TAG, "routes: " + (routes.isEmpty() ? "none" : "some") + ", all on the interface " + sameInterface
            + ", a default route " + defaultRoute + ", its gateway a wildcard " + gatewayWildcard);

        boolean dnsWildcard = false;
        for (InetAddress server : link.getDnsServers()) {
            dnsWildcard |= server.isAnyLocalAddress();
        }
        Log.i(TAG, "dns servers: wildcard " + dnsWildcard);

        String text = link.toString();
        boolean listed = text.contains("InterfaceName: " + name);
        for (LinkAddress address : addresses) {
            listed &= text.contains(address.toString());
        }
        for (RouteInfo route : routes) {
            listed &= text.contains(route.toString());
        }
        Log.i(TAG, "toString lists what the getters hold: " + listed);
        Log.i(TAG, "toString shape: " + text.startsWith("{InterfaceName: " + name + " LinkAddresses: [")
            + " " + text.endsWith("]}"));
        Log.i(TAG, "lists unmodifiable: " + unmodifiable(addresses) + " " + unmodifiable(routes)
            + " " + unmodifiable(link.getDnsServers()));
        Log.i(TAG, "second query equals the first: " + link.equals(manager.getLinkProperties(active)));
    }

    private static boolean unmodifiable(List<?> list) {
        try {
            list.clear();
            return false;
        } catch (UnsupportedOperationException e) {
            return true;
        }
    }
}
