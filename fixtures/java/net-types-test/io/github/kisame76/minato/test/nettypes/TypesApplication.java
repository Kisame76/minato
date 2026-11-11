package io.github.kisame76.minato.test.nettypes;

import android.app.Application;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.IpPrefix;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.RouteInfo;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.Log;

import java.net.InetAddress;

/**
 * The network value types' test: IpPrefix, LinkAddress and RouteInfo, built from addresses, with what they answer and
 * how they print, compare and travel in a Parcel, then what LinkProperties of the active network hands out (those
 * lines are the device's own and only shown).
 */
public class TypesApplication extends Application {
    private static String TAG = "net-types-test";

    @Override
    public void onCreate() {
        super.onCreate();
        TAG = getApplicationInfo().loadLabel(getPackageManager()).toString();
        new Thread(this::run).start();
    }

    private void run() {
        try {
            prefixes();
            addresses();
            routes();
            linkProperties();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
        }
        Log.i(TAG, "done");
    }

    private static InetAddress ip(String text) throws Exception {
        return InetAddress.getByName(text);
    }

    private static <T extends Parcelable> T roundTrip(T value) {
        Parcel parcel = Parcel.obtain();
        try {
            parcel.writeParcelable(value, 0);
            parcel.setDataPosition(0);
            return parcel.readParcelable(value.getClass().getClassLoader());
        } finally {
            parcel.recycle();
        }
    }

    private void prefixes() throws Exception {
        IpPrefix v4 = new IpPrefix(ip("192.168.7.9"), 24);
        IpPrefix v6 = new IpPrefix(ip("2001:db8::1"), 32);
        for (IpPrefix p : new IpPrefix[] {v4, v6}) {
            Log.i(TAG, "prefix " + p + ": address " + p.getAddress().getHostAddress() + ", length "
                + p.getPrefixLength() + ", raw " + p.getRawAddress().length + " bytes");
        }
        Log.i(TAG, "prefix contains 192.168.7.200: " + v4.contains(ip("192.168.7.200")));
        Log.i(TAG, "prefix contains 192.168.8.1: " + v4.contains(ip("192.168.8.1")));
        Log.i(TAG, "prefix contains 2001:db8:ffff::1: " + v6.contains(ip("2001:db8:ffff::1")));
        Log.i(TAG, "prefix contains 2001:db9::1: " + v6.contains(ip("2001:db9::1")));
        Log.i(TAG, "prefix equals " + v4.equals(new IpPrefix(ip("192.168.7.0"), 24)) + " "
            + v4.equals(v6) + ", hash " + (v4.hashCode() == new IpPrefix(ip("192.168.7.77"), 24).hashCode()));
        Log.i(TAG, "prefix parcel: " + roundTrip(v6) + " " + roundTrip(v6).equals(v6));
        try {
            new IpPrefix(ip("10.0.0.1"), 33);
            Log.i(TAG, "prefix 33: accepted");
        } catch (RuntimeException e) {
            Log.i(TAG, "prefix 33: " + e.getClass().getName());
        }
    }

    private void addresses() throws Exception {
        LinkAddress v4 = new LinkAddress(ip("10.1.2.3"), 24);
        LinkAddress v6 = new LinkAddress("2001:db8::5/64");
        for (LinkAddress a : new LinkAddress[] {v4, v6}) {
            Log.i(TAG, "address " + a + ": " + a.getAddress().getHostAddress() + "/" + a.getPrefixLength()
                + ", flags " + a.getFlags() + ", scope " + a.getScope() + ", v4 " + a.isIpv4() + ", v6 " + a.isIpv6());
        }
        Log.i(TAG, "address same prefix: " + v4.isSameAddressAs(new LinkAddress(ip("10.1.2.3"), 16)));
        Log.i(TAG, "address equals " + v4.equals(new LinkAddress("10.1.2.3/24")) + " " + v4.equals(v6)
            + ", hash " + (v4.hashCode() == new LinkAddress("10.1.2.3/24").hashCode()));
        Log.i(TAG, "address parcel: " + roundTrip(v6) + " " + roundTrip(v6).equals(v6));
        try {
            new LinkAddress("nonsense");
            Log.i(TAG, "address nonsense: accepted");
        } catch (RuntimeException e) {
            Log.i(TAG, "address nonsense: " + e.getClass().getName());
        }
    }

    private void routes() throws Exception {
        RouteInfo defaultRoute = new RouteInfo(new IpPrefix(ip("0.0.0.0"), 0), ip("10.1.2.1"), "wlan0",
            RouteInfo.RTN_UNICAST);
        RouteInfo onLink = new RouteInfo(new IpPrefix(ip("10.1.2.0"), 24), null, null, RouteInfo.RTN_UNICAST);
        RouteInfo v6 = new RouteInfo(new IpPrefix(ip("::"), 0), ip("fe80::1"), "wlan0",
            RouteInfo.RTN_UNICAST);
        for (RouteInfo r : new RouteInfo[] {defaultRoute, onLink, v6}) {
            Log.i(TAG, "route " + r + ": destination " + r.getDestination() + ", gateway "
                + (r.getGateway() == null ? "null" : r.getGateway().getHostAddress()) + ", interface "
                + r.getInterface() + ", hasGateway " + r.hasGateway() + ", default " + r.isDefaultRoute()
                + ", type " + r.getType());
        }
        Log.i(TAG, "route matches: " + defaultRoute.matches(ip("8.8.8.8")) + " " + onLink.matches(ip("10.1.2.77"))
            + " " + onLink.matches(ip("10.1.3.1")) + " " + v6.matches(ip("2001:db8::9")));
        Log.i(TAG, "route equals " + defaultRoute.equals(new RouteInfo(new IpPrefix(ip("0.0.0.0"), 0),
            ip("10.1.2.1"), "wlan0", RouteInfo.RTN_UNICAST)) + " " + defaultRoute.equals(onLink) + ", hash "
            + (onLink.hashCode() == new RouteInfo(new IpPrefix(ip("10.1.2.0"), 24), null, null,
            RouteInfo.RTN_UNICAST).hashCode()));
        Log.i(TAG, "route parcel: " + roundTrip(defaultRoute) + " " + roundTrip(defaultRoute).equals(defaultRoute));
    }

    /**
     * What LinkProperties of the active network hands out: the device's lines, only shown. A phone with a network gives
     * its interface, addresses, routes and DNS servers; Minato's LinkProperties is written and empty, and the harness
     * has no host network (a known gap, named in the landing).
     */
    private void linkProperties() {
        String device = TAG + "-device";
        ConnectivityManager manager = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        Network active = manager.getActiveNetwork();
        Log.i(device, "active network: " + (active != null));
        LinkProperties properties = active == null ? null : manager.getLinkProperties(active);
        Log.i(device, "link properties: " + (properties != null));
        if (properties == null) return;
        Log.i(device, "interface " + properties.getInterfaceName());
        Log.i(device, "addresses " + properties.getLinkAddresses());
        Log.i(device, "routes " + properties.getRoutes());
        Log.i(device, "dns " + properties.getDnsServers());
    }
}
