// The written part of LinkProperties (ADR-0054 step d)), which tools/managergen joins with the API's methods: a
// network's interface name, addresses, DNS servers, routes and MTU, which ConnectivityManager gives it from the
// runtime's snapshot of the host's active network (LocalNetwork.link, APKRT_NETWORK_LINK), the same data the guest's
// getifaddrs and netlink answer from; toString prints as AOSP's (the fields the snapshot has, the others as an unset
// LinkProperties).
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.StringJoiner;

class LinkProperties {
    private String mIfaceName;
    private final ArrayList<LinkAddress> mLinkAddresses = new ArrayList<>();
    private final ArrayList<InetAddress> mDnses = new ArrayList<>();
    private final ArrayList<RouteInfo> mRoutes = new ArrayList<>();
    private int mMtu;

    public LinkProperties() {
    }

    public String getInterfaceName() {
        return mIfaceName;
    }

    public void setInterfaceName(String name) {
        mIfaceName = name;
    }

    /** @hide As AOSP's: false for an address the link has already. */
    public boolean addLinkAddress(LinkAddress address) {
        if (address == null || mLinkAddresses.contains(address)) return false;
        mLinkAddresses.add(address);
        return true;
    }

    public List<LinkAddress> getLinkAddresses() {
        return Collections.unmodifiableList(new ArrayList<>(mLinkAddresses));
    }

    /** @hide As AOSP's: false for a server the link has already. */
    public boolean addDnsServer(InetAddress server) {
        if (server == null || mDnses.contains(server)) return false;
        mDnses.add(server);
        return true;
    }

    public List<InetAddress> getDnsServers() {
        return Collections.unmodifiableList(new ArrayList<>(mDnses));
    }

    /** @hide As AOSP's: false for a route the link has already. */
    public boolean addRoute(RouteInfo route) {
        if (route == null || mRoutes.contains(route)) return false;
        mRoutes.add(route);
        return true;
    }

    public List<RouteInfo> getRoutes() {
        return Collections.unmodifiableList(new ArrayList<>(mRoutes));
    }

    public void setMtu(int mtu) {
        mMtu = mtu;
    }

    public int getMtu() {
        return mMtu;
    }

    public int describeContents() {
        return 0;
    }

    /** @hide As AOSP's. */
    @Override
    public boolean equals(Object other) {
        if (!(other instanceof LinkProperties)) return false;
        LinkProperties that = (LinkProperties) other;
        return java.util.Objects.equals(mIfaceName, that.mIfaceName) && mLinkAddresses.equals(that.mLinkAddresses)
                && mDnses.equals(that.mDnses) && mRoutes.equals(that.mRoutes) && mMtu == that.mMtu;
    }

    /** @hide As AOSP's. */
    @Override
    public int hashCode() {
        return java.util.Objects.hash(mIfaceName, mLinkAddresses, mDnses, mRoutes, mMtu);
    }

    /** @hide As AOSP's: its fields in its order, an unset one printed as AOSP prints it (Domains: null). */
    @Override
    public String toString() {
        final StringJoiner joiner = new StringJoiner(" ", "{", "}");
        if (mIfaceName != null) {
            joiner.add("InterfaceName:");
            joiner.add(mIfaceName);
        }
        joiner.add("LinkAddresses: [");
        if (!mLinkAddresses.isEmpty()) joiner.add(join(mLinkAddresses));
        joiner.add("]");
        joiner.add("DnsAddresses: [");
        if (!mDnses.isEmpty()) joiner.add(join(mDnses));
        joiner.add("]");
        joiner.add("Domains:");
        joiner.add(null);
        joiner.add("MTU:");
        joiner.add(Integer.toString(mMtu));
        joiner.add("Routes: [");
        if (!mRoutes.isEmpty()) joiner.add(join(mRoutes));
        joiner.add("]");
        return joiner.toString();
    }

    private static String join(List<?> items) {
        StringJoiner joiner = new StringJoiner(",");
        for (Object item : items) joiner.add(String.valueOf(item));
        return joiner.toString();
    }
}
