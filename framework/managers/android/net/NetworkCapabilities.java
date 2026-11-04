// The written part of NetworkCapabilities (ADR-0054 step d)), which tools/managergen joins with the API's methods: a
// network's transports and capabilities as bit sets, as AOSP's (connectivity's NetworkCapabilities.java at
// android-15.0.0_r1) keeps them, with its defaults (NOT_RESTRICTED, TRUSTED, NOT_VPN and, from Android 15,
// NOT_BANDWIDTH_CONSTRAINED) and the hidden setters ConnectivityManager uses (LocalNetwork), with the forbidden
// capabilities and the network specifier a NetworkRequest keeps and AOSP's checks of whether a network satisfies one.
// No uids, SSID, subscription or enterprise IDs: a request without them, as an app builds it, and Minato's network
// without them satisfy each other there, as AOSP's checks answer for none.
import android.util.Log;

class NetworkCapabilities {
    private static final String TAG = "NetworkCapabilities";
    private static final long DEFAULT_CAPABILITIES = (1L << NET_CAPABILITY_NOT_RESTRICTED)
            | (1L << NET_CAPABILITY_TRUSTED) | (1L << NET_CAPABILITY_NOT_VPN)
            | (1L << NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED);
    /** As AOSP's MIN_ and MAX_NET_CAPABILITY and MIN_ and MAX_TRANSPORT. */
    private static final int MAX_NET_CAPABILITY = NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED;
    private static final int MAX_TRANSPORT = TRANSPORT_SATELLITE;
    /** AOSP's hidden TRANSPORT_TEST. */
    private static final int TRANSPORT_TEST = 7;

    private long mNetworkCapabilities;
    private long mForbiddenNetworkCapabilities;
    private long mTransportTypes;
    private NetworkSpecifier mNetworkSpecifier;
    private int mLinkUpBandwidthKbps;
    private int mLinkDownBandwidthKbps;
    private int mSignalStrength = SIGNAL_STRENGTH_UNSPECIFIED;

    public NetworkCapabilities() {
        mNetworkCapabilities = DEFAULT_CAPABILITIES;
    }

    public NetworkCapabilities(NetworkCapabilities other) {
        if (other != null) {
            mNetworkCapabilities = other.mNetworkCapabilities;
            mForbiddenNetworkCapabilities = other.mForbiddenNetworkCapabilities;
            mTransportTypes = other.mTransportTypes;
            mNetworkSpecifier = other.mNetworkSpecifier;
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

    /** @hide As AOSP's: no longer forbidden once added; an invalid one is logged and left. */
    public NetworkCapabilities addCapability(int capability) {
        if (!isValidCapability(capability)) {
            Log.e(TAG, "addCapability is called with invalid capability: " + capability);
            return this;
        }
        mNetworkCapabilities |= 1L << capability;
        mForbiddenNetworkCapabilities &= ~(1L << capability);
        return this;
    }

    /** @hide As AOSP's: no longer wanted. */
    public NetworkCapabilities removeCapability(int capability) {
        if (!isValidCapability(capability)) {
            Log.e(TAG, "removeCapability is called with invalid capability: " + capability);
            return this;
        }
        mNetworkCapabilities &= ~(1L << capability);
        return this;
    }

    /** @hide As AOSP's: no longer wanted once forbidden. */
    public void addForbiddenCapability(int capability) {
        if (!isValidCapability(capability)) {
            Log.e(TAG, "addForbiddenCapability is called with invalid capability: " + capability);
            return;
        }
        mForbiddenNetworkCapabilities |= 1L << capability;
        mNetworkCapabilities &= ~(1L << capability);
    }

    /** @hide As AOSP's. */
    public NetworkCapabilities removeForbiddenCapability(int capability) {
        if (!isValidCapability(capability)) {
            Log.e(TAG, "removeForbiddenCapability is called with invalid capability: " + capability);
            return this;
        }
        mForbiddenNetworkCapabilities &= ~(1L << capability);
        return this;
    }

    /** @hide As AOSP's. */
    public boolean hasForbiddenCapability(int capability) {
        return capability >= 0 && capability <= MAX_NET_CAPABILITY
                && (mForbiddenNetworkCapabilities & (1L << capability)) != 0;
    }

    /** @hide As AOSP's. */
    public int[] getForbiddenCapabilities() {
        return unpack(mForbiddenNetworkCapabilities);
    }

    /** @hide As AOSP's: nothing wanted, forbidden or given. */
    public void clearAll() {
        mNetworkCapabilities = mTransportTypes = mForbiddenNetworkCapabilities = 0;
        mLinkUpBandwidthKbps = mLinkDownBandwidthKbps = 0;
        mNetworkSpecifier = null;
        mSignalStrength = SIGNAL_STRENGTH_UNSPECIFIED;
    }

    /** @hide As AOSP's: the specifier checked again. */
    public NetworkCapabilities addTransportType(int transportType) {
        checkValidTransportType(transportType);
        mTransportTypes |= 1L << transportType;
        setNetworkSpecifier(mNetworkSpecifier);
        return this;
    }

    /** @hide As AOSP's: the specifier checked again. */
    public NetworkCapabilities removeTransportType(int transportType) {
        checkValidTransportType(transportType);
        mTransportTypes &= ~(1L << transportType);
        setNetworkSpecifier(mNetworkSpecifier);
        return this;
    }

    /** @hide As AOSP's. */
    public NetworkCapabilities setLinkUpstreamBandwidthKbps(int upKbps) {
        mLinkUpBandwidthKbps = upKbps;
        return this;
    }

    /** @hide As AOSP's. */
    public NetworkCapabilities setLinkDownstreamBandwidthKbps(int downKbps) {
        mLinkDownBandwidthKbps = downKbps;
        return this;
    }

    /** @hide As AOSP's. */
    public NetworkCapabilities setSignalStrength(int signalStrength) {
        mSignalStrength = signalStrength;
        return this;
    }

    /**
     * @hide As AOSP's: a specifier needs the test transport alone, a single other one besides it, or cellular and
     * satellite, which share TelephonyNetworkSpecifier.
     */
    public NetworkCapabilities setNetworkSpecifier(NetworkSpecifier networkSpecifier) {
        long others = mTransportTypes & ~(1L << TRANSPORT_TEST);
        if (networkSpecifier != null && mTransportTypes != (1L << TRANSPORT_TEST) && Long.bitCount(others) != 1
                && others != (1L << TRANSPORT_CELLULAR | 1L << TRANSPORT_SATELLITE)) {
            throw new IllegalStateException("Must have a single non-test transport specified to "
                    + "use setNetworkSpecifier");
        }
        mNetworkSpecifier = networkSpecifier;
        return this;
    }

    public NetworkSpecifier getNetworkSpecifier() {
        return mNetworkSpecifier;
    }

    /**
     * @hide As AOSP's satisfiedByNetworkCapabilities: the wanted capabilities all there and no forbidden one, a
     * transport of the wanted ones (any without one), the bandwidths and the signal at least as asked, and the
     * specifier satisfied. A specifier's own canBeSatisfiedBy is hidden; those of the API (Telephony's, Ethernet's,
     * Wi-Fi's) take an equal one only, which is what it asks here, and none takes a network without one.
     */
    public boolean satisfiedByNetworkCapabilities(NetworkCapabilities nc) {
        return nc != null
                && (nc.mNetworkCapabilities & mNetworkCapabilities) == mNetworkCapabilities
                && (mForbiddenNetworkCapabilities & nc.mNetworkCapabilities) == 0
                && (mTransportTypes == 0 || (mTransportTypes & nc.mTransportTypes) != 0)
                && mLinkUpBandwidthKbps <= nc.mLinkUpBandwidthKbps
                && mLinkDownBandwidthKbps <= nc.mLinkDownBandwidthKbps
                && (mNetworkSpecifier == null || mNetworkSpecifier.equals(nc.mNetworkSpecifier)
                        || nc.mNetworkSpecifier instanceof MatchAllNetworkSpecifier)
                && mSignalStrength <= nc.mSignalStrength;
    }

    private static boolean isValidCapability(int capability) {
        return capability >= 0 && capability <= MAX_NET_CAPABILITY;
    }

    private static void checkValidTransportType(int transport) {
        if (transport < 0 || transport > MAX_TRANSPORT) {
            throw new IllegalArgumentException("Invalid TransportType " + transport);
        }
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
        return mNetworkCapabilities == that.mNetworkCapabilities
                && mForbiddenNetworkCapabilities == that.mForbiddenNetworkCapabilities
                && mTransportTypes == that.mTransportTypes
                && java.util.Objects.equals(mNetworkSpecifier, that.mNetworkSpecifier)
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
