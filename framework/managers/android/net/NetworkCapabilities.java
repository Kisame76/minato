// The written part of NetworkCapabilities (ADR-0054 step d)), which tools/managergen joins with the API's methods: a
// network's transports and capabilities as bit sets, as AOSP's (connectivity's NetworkCapabilities.java at
// android-15.0.0_r1) keeps them, with its defaults (NOT_RESTRICTED, TRUSTED, NOT_VPN and, from Android 15,
// NOT_BANDWIDTH_CONSTRAINED) and the hidden setters ConnectivityManager uses (LocalNetwork).
class NetworkCapabilities {
    private static final long DEFAULT_CAPABILITIES = (1L << NET_CAPABILITY_NOT_RESTRICTED)
            | (1L << NET_CAPABILITY_TRUSTED) | (1L << NET_CAPABILITY_NOT_VPN)
            | (1L << NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED);
    /** As AOSP's MIN_ and MAX_NET_CAPABILITY and MIN_ and MAX_TRANSPORT. */
    private static final int MAX_NET_CAPABILITY = NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED;
    private static final int MAX_TRANSPORT = TRANSPORT_SATELLITE;

    private long mNetworkCapabilities;
    private long mTransportTypes;
    private int mLinkUpBandwidthKbps;
    private int mLinkDownBandwidthKbps;
    private int mSignalStrength = SIGNAL_STRENGTH_UNSPECIFIED;

    public NetworkCapabilities() {
        mNetworkCapabilities = DEFAULT_CAPABILITIES;
    }

    public NetworkCapabilities(NetworkCapabilities other) {
        if (other != null) {
            mNetworkCapabilities = other.mNetworkCapabilities;
            mTransportTypes = other.mTransportTypes;
            mLinkUpBandwidthKbps = other.mLinkUpBandwidthKbps;
            mLinkDownBandwidthKbps = other.mLinkDownBandwidthKbps;
            mSignalStrength = other.mSignalStrength;
        }
    }

    public boolean hasCapability(int capability) {
        return capability >= 0 && capability <= MAX_NET_CAPABILITY && (mNetworkCapabilities & (1L << capability)) != 0;
    }

    public boolean hasTransport(int transportType) {
        return transportType >= 0 && transportType <= MAX_TRANSPORT && (mTransportTypes & (1L << transportType)) != 0;
    }

    public int[] getCapabilities() {
        return unpack(mNetworkCapabilities);
    }

    /** @hide As AOSP's. */
    public int[] getTransportTypes() {
        return unpack(mTransportTypes);
    }

    private static int[] unpack(long bits) {
        int[] values = new int[Long.bitCount(bits)];
        int index = 0;
        for (int bit = 0; bit < 64; bit++) {
            if ((bits & (1L << bit)) != 0) values[index++] = bit;
        }
        return values;
    }

    /** @hide As AOSP's. */
    public NetworkCapabilities addCapability(int capability) {
        mNetworkCapabilities |= 1L << capability;
        return this;
    }

    /** @hide As AOSP's. */
    public NetworkCapabilities removeCapability(int capability) {
        mNetworkCapabilities &= ~(1L << capability);
        return this;
    }

    /** @hide As AOSP's. */
    public NetworkCapabilities addTransportType(int transportType) {
        mTransportTypes |= 1L << transportType;
        return this;
    }

    public int getLinkUpstreamBandwidthKbps() {
        return mLinkUpBandwidthKbps;
    }

    public int getLinkDownstreamBandwidthKbps() {
        return mLinkDownBandwidthKbps;
    }

    public int getSignalStrength() {
        return mSignalStrength;
    }

    public TransportInfo getTransportInfo() {
        return null;
    }

    /** As AOSP's for an app that owns no network: Process.INVALID_UID. */
    public int getOwnerUid() {
        return -1;
    }

    public int describeContents() {
        return 0;
    }

    /** @hide As AOSP's. */
    @Override
    public boolean equals(Object other) {
        if (!(other instanceof NetworkCapabilities)) return false;
        NetworkCapabilities that = (NetworkCapabilities) other;
        return mNetworkCapabilities == that.mNetworkCapabilities && mTransportTypes == that.mTransportTypes
                && mLinkUpBandwidthKbps == that.mLinkUpBandwidthKbps
                && mLinkDownBandwidthKbps == that.mLinkDownBandwidthKbps && mSignalStrength == that.mSignalStrength;
    }

    /** @hide As AOSP's. */
    @Override
    public int hashCode() {
        return Long.hashCode(mNetworkCapabilities) * 31 + Long.hashCode(mTransportTypes);
    }

    /** @hide As AOSP's. */
    @Override
    public String toString() {
        return "[ Transports: " + bits(mTransportTypes) + " Capabilities: " + bits(mNetworkCapabilities)
                + " LinkUpBandwidth>=" + mLinkUpBandwidthKbps + "Kbps LinkDnBandwidth>=" + mLinkDownBandwidthKbps
                + "Kbps]";
    }

    private static String bits(long bits) {
        StringBuilder text = new StringBuilder();
        for (int value : unpack(bits)) {
            if (text.length() > 0) text.append('&');
            text.append(value);
        }
        return text.toString();
    }
}
