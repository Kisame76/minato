package com.android.net.module.util;

import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;

/**
 * Ours in place of the connectivity module's NetworkStackConstants (not fetched): the two wildcard addresses that
 * RouteInfo uses.
 */
public final class NetworkStackConstants {
    private NetworkStackConstants() {
    }

    public static final Inet4Address IPV4_ADDR_ANY = any4();
    public static final Inet6Address IPV6_ADDR_ANY = any6();

    private static Inet4Address any4() {
        try {
            return (Inet4Address) InetAddress.getByAddress(new byte[] {0, 0, 0, 0});
        } catch (UnknownHostException e) {
            throw new IllegalStateException(e);
        }
    }

    private static Inet6Address any6() {
        try {
            return (Inet6Address) InetAddress.getByAddress(new byte[16]);
        } catch (UnknownHostException e) {
            throw new IllegalStateException(e);
        }
    }
}
