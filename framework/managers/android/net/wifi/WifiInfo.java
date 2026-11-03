// The written part of WifiInfo (ADR-0054 step d)), which tools/managergen joins with the API's methods: the Wi-Fi
// connection as an app without a location permission sees it, as AOSP's (wifi's WifiInfo.java at android-15.0.0_r1)
// redacts it: no SSID ("<unknown ssid>"), the default MAC address for the BSSID and the device's own, no network ID;
// the device's IPv4 address. The link speeds, the frequency and the signal are unknown, which iOS does not tell an app.
class WifiInfo {
    /** AOSP's hidden DEFAULT_MAC_ADDRESS. */
    private static final String REDACTED_MAC_ADDRESS = "02:00:00:00:00:00";
    /** AOSP's hidden INVALID_RSSI. */
    private static final int UNKNOWN_RSSI = -127;

    private SupplicantState mSupplicantState = SupplicantState.UNINITIALIZED;
    private int mIpAddress;

    /** @hide As AOSP's: no connection. */
    public WifiInfo() {
    }

    /** @hide LocalNetwork's: a completed connection at `ipAddress` (AOSP's int, its first byte lowest). */
    public static WifiInfo connected(int ipAddress) {
        WifiInfo info = new WifiInfo();
        info.mSupplicantState = SupplicantState.COMPLETED;
        info.mIpAddress = ipAddress;
        return info;
    }

    public String getSSID() {
        return WifiManager.UNKNOWN_SSID;
    }

    public String getBSSID() {
        return mSupplicantState == SupplicantState.COMPLETED ? REDACTED_MAC_ADDRESS : null;
    }

    public String getMacAddress() {
        return REDACTED_MAC_ADDRESS;
    }

    public int getNetworkId() {
        return -1;
    }

    public boolean getHiddenSSID() {
        return false;
    }

    public SupplicantState getSupplicantState() {
        return mSupplicantState;
    }

    public int getIpAddress() {
        return mIpAddress;
    }

    public int getRssi() {
        return UNKNOWN_RSSI;
    }

    public int getLinkSpeed() {
        return LINK_SPEED_UNKNOWN;
    }

    public int getTxLinkSpeedMbps() {
        return LINK_SPEED_UNKNOWN;
    }

    public int getRxLinkSpeedMbps() {
        return LINK_SPEED_UNKNOWN;
    }

    public int getFrequency() {
        return -1;
    }

    public int describeContents() {
        return 0;
    }
}
