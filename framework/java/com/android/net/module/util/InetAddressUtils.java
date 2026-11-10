package com.android.net.module.util;

import android.os.Parcel;

import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;

/**
 * Writing an InetAddress into a Parcel and reading it back, for the connectivity module's parcelables that carry one
 * (NsdServiceInfo). Ours, in the module's format: the address bytes, then the scope of an IPv6 address
 * [inferred from android-15.0.0_r1, InetAddressUtils.java].
 */
public final class InetAddressUtils {
    private InetAddressUtils() {
    }

    public static void parcelInetAddress(Parcel parcel, InetAddress address, int flags) {
        parcel.writeByteArray(address != null ? address.getAddress() : null);
        parcel.writeInt(address instanceof Inet6Address ? ((Inet6Address) address).getScopeId() : 0);
    }

    public static InetAddress unparcelInetAddress(Parcel in) {
        final byte[] bytes = in.createByteArray();
        final int scope = in.readInt();
        if (bytes == null) return null;
        try {
            return bytes.length == 16 && scope != 0 ? Inet6Address.getByAddress(null, bytes, scope)
                    : InetAddress.getByAddress(bytes);
        } catch (UnknownHostException e) {
            return null;
        }
    }
}
