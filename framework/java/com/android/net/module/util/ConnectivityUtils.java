package com.android.net.module.util;

import java.net.Inet6Address;
import java.net.InetAddress;

/**
 * Ours in place of the connectivity module's ConnectivityUtils (not fetched): the unique local address test that
 * LinkAddress makes.
 */
public final class ConnectivityUtils {
    private ConnectivityUtils() {
    }

    /** Whether the address is an IPv6 unique local address (fc00::/7). */
    public static boolean isIPv6ULA(InetAddress addr) {
        return addr instanceof Inet6Address && (addr.getAddress()[0] & 0xfe) == 0xfc;
    }
}
