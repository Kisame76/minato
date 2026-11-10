package com.android.net.module.util;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.UnknownHostException;

/**
 * Ours in place of the connectivity module's com.android.net.module.util.Inet4AddressUtils (packages/modules/
 * Connectivity's staticlibs), which is not fetched: the conversions of an IPv4 address between an int and its four
 * bytes that android.text.format.Formatter and the framework's network classes use, with the module's names and
 * results. HTL is "host to ..." with the address's first byte in the int's lowest byte (0x0100007f is 127.0.0.1),
 * HTH with the first byte in the highest (0x7f000001 is 127.0.0.1).
 */
public class Inet4AddressUtils {
    private Inet4AddressUtils() {}

    /** Converts an int whose lowest byte is the address's first byte (0x0100007f: 127.0.0.1) to the address. */
    public static Inet4Address intToInet4AddressHTL(int hostAddress) {
        return intToInet4AddressHTH(Integer.reverseBytes(hostAddress));
    }

    /** Converts an int whose highest byte is the address's first byte (0x7f000001: 127.0.0.1) to the address. */
    public static Inet4Address intToInet4AddressHTH(int hostAddress) {
        final byte[] addressBytes = {(byte) (0xff & (hostAddress >> 24)), (byte) (0xff & (hostAddress >> 16)),
                (byte) (0xff & (hostAddress >> 8)), (byte) (0xff & hostAddress)};
        try {
            return (Inet4Address) InetAddress.getByAddress(addressBytes);
        } catch (UnknownHostException e) {
            throw new AssertionError("Impossible: four bytes are an address", e);
        }
    }

    /** The address's first byte in the int's lowest byte, the reverse of intToInet4AddressHTL. */
    public static int inet4AddressToIntHTL(Inet4Address inetAddr) throws IllegalArgumentException {
        return Integer.reverseBytes(inet4AddressToIntHTH(inetAddr));
    }

    /** The address's first byte in the int's highest byte, the reverse of intToInet4AddressHTH. */
    public static int inet4AddressToIntHTH(Inet4Address inetAddr) throws IllegalArgumentException {
        final byte[] addr = inetAddr.getAddress();
        return ((addr[0] & 0xff) << 24) | ((addr[1] & 0xff) << 16) | ((addr[2] & 0xff) << 8) | (addr[3] & 0xff);
    }

    /** The prefix length of a netmask given as an int in network order's HTH form; rejects a mask with holes. */
    public static int netmaskToPrefixLength(Inet4Address netmask) {
        final int i = inet4AddressToIntHTH(netmask);
        final int prefixLength = Integer.bitCount(i);
        final int trailingZeros = Integer.numberOfTrailingZeros(i);
        if (trailingZeros != 32 - prefixLength) {
            throw new IllegalArgumentException("Non-contiguous netmask: " + Integer.toHexString(i));
        }
        return prefixLength;
    }

    /** The netmask of a prefix length from 0 to 32 as an int in HTH form. */
    public static int prefixLengthToV4NetmaskIntHTH(int prefixLength) throws IllegalArgumentException {
        if (prefixLength < 0 || prefixLength > 32) {
            throw new IllegalArgumentException("Invalid prefix length (0 <= prefix <= 32): " + prefixLength);
        }
        return prefixLength == 0 ? 0 : 0xffffffff << (32 - prefixLength);
    }
}
