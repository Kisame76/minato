package com.android.net.module.util;

import android.net.RouteInfo;

import java.net.InetAddress;
import java.util.Collection;

/**
 * Ours in place of the connectivity module's com.android.net.module.util.NetUtils (not fetched): the two calls that
 * IpPrefix and RouteInfo make. maskRawAddress clears every bit of the address after the first prefixLength bits;
 * selectBestRoute takes, of the routes that match the destination, the one with the longest prefix [inferred from
 * RouteInfo's own contract: "the best match" for a destination, as LinkProperties.route lookup uses it].
 */
public final class NetUtils {
    private NetUtils() {
    }

    public static void maskRawAddress(byte[] array, int prefixLength) {
        if (prefixLength < 0 || prefixLength > array.length * 8) {
            throw new RuntimeException("IP address with " + array.length + " bytes has invalid prefix length "
                + prefixLength);
        }
        int offset = prefixLength / 8;
        int remainder = prefixLength % 8;
        byte mask = (byte) (0xFF << (8 - remainder));
        if (offset < array.length) array[offset] = (byte) (array[offset] & mask);
        offset++;
        for (; offset < array.length; offset++) {
            array[offset] = 0;
        }
    }

    public static RouteInfo selectBestRoute(Collection<RouteInfo> routes, InetAddress dest) {
        if (routes == null || dest == null) return null;
        RouteInfo best = null;
        for (RouteInfo route : routes) {
            if (!route.matches(dest)) continue;
            if (best == null || best.getDestination().getPrefixLength() < route.getDestination().getPrefixLength()) {
                best = route;
            }
        }
        return best;
    }
}
