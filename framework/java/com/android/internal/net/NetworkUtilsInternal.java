/*
 * Written; see below. AOSP's file is Apache 2.0, (c) The Android Open Source Project.
 */
package com.android.internal.net;

import static android.system.OsConstants.AF_INET;
import static android.system.OsConstants.AF_INET6;

import android.annotation.NonNull;
import android.system.Os;

import java.io.FileDescriptor;

/** @hide */
/**
 * The written part of NetworkUtilsInternal (group 1 of the framework breadth): AOSP's natives ask netd to protect a
 * socket from the VPN and to switch an app's networking off, and Minato has neither a VPN nor a netd. An app the user
 * has not allowed to be a VPN is refused protect() as on a phone (false) [verified: vpn-test, protect(-1)]; the
 * networking switch is a no-op, as no app here is restricted from the network by the framework.
 */
public class NetworkUtilsInternal {

    private static final int[] ADDRESS_FAMILIES = new int[] {AF_INET, AF_INET6};

    public static void setAllowNetworkingForProcess(boolean allowNetworking) {
    }

    public static boolean protectFromVpn(FileDescriptor fd) {
        return false;
    }

    public static boolean protectFromVpn(int socketfd) {
        return false;
    }

    /** @hide */
    public static boolean isWeaklyValidatedHostname(@NonNull String hostname) {
        final String weakHostnameRegex = "^[a-zA-Z0-9_.-]+$";
        if (!hostname.matches(weakHostnameRegex)) {
            return false;
        }
        for (int address_family : ADDRESS_FAMILIES) {
            if (Os.inet_pton(address_family, hostname) != null) {
                return false;
            }
        }
        return true;
    }
}
