/*
 * Written in place of the connectivity module's android.net.NetworkUtils (hidden), whose natives ask netd to bind
 * sockets to networks, send DNS queries and read TCP repair windows: nothing of that exists here. IpPrefix and
 * LinkAddress parse "address/prefix" strings through legacyParseIpAndMask, which is AOSP's own code.
 */
package android.net;

import android.util.Pair;

import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.UnknownHostException;

public class NetworkUtils {
    private NetworkUtils() {
    }

    /** Parses "address/prefix" (also "[v6]/prefix" and "/prefix" for ::1), as AOSP's legacy parser does. */
    public static Pair<InetAddress, Integer> legacyParseIpAndMask(String ipAndMaskString) {
        InetAddress address = null;
        int prefixLength = -1;
        try {
            String[] pieces = ipAndMaskString.split("/", 2);
            prefixLength = Integer.parseInt(pieces[1]);
            if (pieces[0] == null || pieces[0].isEmpty()) {
                final byte[] bytes = new byte[16];
                bytes[15] = 1;
                return new Pair<InetAddress, Integer>(Inet6Address.getByAddress("ip6-localhost", bytes, 0),
                    prefixLength);
            }
            if (pieces[0].startsWith("[") && pieces[0].endsWith("]") && pieces[0].indexOf(':') != -1) {
                pieces[0] = pieces[0].substring(1, pieces[0].length() - 1);
            }
            address = InetAddresses.parseNumericAddress(pieces[0]);
        } catch (NullPointerException e) {            // Null string.
        } catch (ArrayIndexOutOfBoundsException e) {  // No prefix length.
        } catch (NumberFormatException e) {           // Non-numeric prefix.
        } catch (IllegalArgumentException e) {        // Invalid IP address.
        } catch (UnknownHostException e) {            // IP address length is illegal.
        }
        if (address == null || prefixLength == -1) {
            throw new IllegalArgumentException("Invalid IP address and mask " + ipAndMaskString);
        }
        return new Pair<InetAddress, Integer>(address, prefixLength);
    }
}
