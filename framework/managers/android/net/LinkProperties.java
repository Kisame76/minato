// The written part of LinkProperties (ADR-0054 step d)), which tools/managergen joins with the API's methods: a
// network's interface name, which ConnectivityManager gives it with the guest's name of the host's interface (wlan0,
// rmnet_data0, eth0, as the runtime's netlink names them), with no addresses, routes or DNS servers listed: the
// guest's getifaddrs and resolver have them.
import java.net.InetAddress;
import java.util.Collections;
import java.util.List;

class LinkProperties {
    private String mIfaceName;

    public LinkProperties() {
    }

    public String getInterfaceName() {
        return mIfaceName;
    }

    public void setInterfaceName(String name) {
        mIfaceName = name;
    }

    public List<LinkAddress> getLinkAddresses() {
        return Collections.emptyList();
    }

    public List<InetAddress> getDnsServers() {
        return Collections.emptyList();
    }

    public List<RouteInfo> getRoutes() {
        return Collections.emptyList();
    }

    public int describeContents() {
        return 0;
    }

    /** @hide As AOSP's. */
    @Override
    public boolean equals(Object other) {
        return other instanceof LinkProperties
                && java.util.Objects.equals(mIfaceName, ((LinkProperties) other).mIfaceName);
    }

    /** @hide As AOSP's. */
    @Override
    public int hashCode() {
        return mIfaceName == null ? 0 : mIfaceName.hashCode();
    }

    /** @hide As AOSP's. */
    @Override
    public String toString() {
        return "{" + (mIfaceName == null ? "" : "InterfaceName: " + mIfaceName + " ")
                + "LinkAddresses: [ ] DnsAddresses: [ ] Routes: [ ]}";
    }
}
