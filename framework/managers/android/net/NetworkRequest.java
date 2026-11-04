// The written part of NetworkRequest (ADR-0054 step d)), which tools/managergen joins with the API's methods: a
// request's capabilities, as AOSP's (connectivity's NetworkRequest.java at android-15.0.0_r1, Apache-2.0) keeps them,
// and its Builder with AOSP's defaults (NetworkCapabilities') and its deduction of NOT_VCN_MANAGED. A built request
// is of no type and has no ID yet, as AOSP's until ConnectivityService takes it; ConnectivityManager's listens and
// requests (LocalNetwork) match it against Minato's network with canBeSatisfiedBy. Not taken: AOSP's restricting of a
// request whose capabilities are all restricted ones (NetworkCapabilitiesUtils, not among the fetched sources): such
// a request keeps NOT_RESTRICTED, which only its getCapabilities shows, as no network of Minato's has those
// capabilities; the requestor's UID and the subscription IDs.
import java.util.Arrays;
import java.util.List;
import java.util.Objects;

class NetworkRequest {
    /** @hide As AOSP's. */
    public final NetworkCapabilities networkCapabilities;

    /** @hide As AOSP's. */
    public NetworkRequest(NetworkCapabilities nc) {
        networkCapabilities = new NetworkCapabilities(nc);
    }

    /** @hide As AOSP's. */
    public NetworkRequest(NetworkRequest that) {
        networkCapabilities = new NetworkCapabilities(that.networkCapabilities);
    }

    public boolean hasCapability(int capability) {
        return networkCapabilities.hasCapability(capability);
    }

    public boolean hasTransport(int transportType) {
        return networkCapabilities.hasTransport(transportType);
    }

    public boolean canBeSatisfiedBy(NetworkCapabilities nc) {
        return networkCapabilities.satisfiedByNetworkCapabilities(nc);
    }

    public NetworkSpecifier getNetworkSpecifier() {
        return networkCapabilities.getNetworkSpecifier();
    }

    public int[] getCapabilities() {
        return networkCapabilities.getCapabilities();
    }

    public int[] getTransportTypes() {
        return networkCapabilities.getTransportTypes();
    }

    /** As AOSP's for a request of no subscription. */
    public java.util.Set<Integer> getSubscriptionIds() {
        return new android.util.ArraySet<>();
    }

    public int describeContents() {
        return 0;
    }

    /** @hide As AOSP's for a request that ConnectivityService has not taken: of type NONE, with ID 0. */
    @Override
    public String toString() {
        return "NetworkRequest [ NONE id=0, " + networkCapabilities + " ]";
    }

    /** @hide As AOSP's: built requests differ in their capabilities only. */
    @Override
    public boolean equals(Object obj) {
        return obj instanceof NetworkRequest
                && Objects.equals(((NetworkRequest) obj).networkCapabilities, networkCapabilities);
    }

    /** @hide As AOSP's. */
    @Override
    public int hashCode() {
        return Objects.hash(0, -1, networkCapabilities, 0);
    }

    public static class Builder {
        /** NetworkCapabilities' system API NET_CAPABILITY_PARTIAL_CONNECTIVITY. */
        private static final int NET_CAPABILITY_PARTIAL_CONNECTIVITY = 24;
        /** NetworkCapabilities' system API NET_CAPABILITY_NOT_VCN_MANAGED. */
        private static final int NET_CAPABILITY_NOT_VCN_MANAGED = 28;
        /** AOSP's VCN_SUPPORTED_CAPABILITIES. */
        private static final List<Integer> VCN_SUPPORTED_CAPABILITIES = Arrays.asList(
                NetworkCapabilities.NET_CAPABILITY_CAPTIVE_PORTAL,
                NetworkCapabilities.NET_CAPABILITY_DUN,
                NetworkCapabilities.NET_CAPABILITY_FOREGROUND,
                NetworkCapabilities.NET_CAPABILITY_INTERNET,
                NetworkCapabilities.NET_CAPABILITY_NOT_CONGESTED,
                NetworkCapabilities.NET_CAPABILITY_NOT_METERED,
                NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED,
                NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING,
                NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED,
                NetworkCapabilities.NET_CAPABILITY_NOT_VPN,
                NET_CAPABILITY_PARTIAL_CONNECTIVITY,
                NetworkCapabilities.NET_CAPABILITY_TEMPORARILY_NOT_METERED,
                NetworkCapabilities.NET_CAPABILITY_TRUSTED,
                NetworkCapabilities.NET_CAPABILITY_VALIDATED,
                NetworkCapabilities.NET_CAPABILITY_NOT_BANDWIDTH_CONSTRAINED);

        private final NetworkCapabilities mNetworkCapabilities;
        private boolean mShouldDeduceNotVcnManaged = true;

        public Builder() {
            mNetworkCapabilities = new NetworkCapabilities();
        }

        /** As AOSP's: the request's capabilities themselves, NOT_VCN_MANAGED left as they have it. */
        public Builder(NetworkRequest request) {
            Objects.requireNonNull(request);
            mNetworkCapabilities = request.networkCapabilities;
            mShouldDeduceNotVcnManaged = false;
        }

        public NetworkRequest build() {
            NetworkCapabilities nc = new NetworkCapabilities(mNetworkCapabilities);
            if (mShouldDeduceNotVcnManaged) {
                boolean supported = true;
                for (int capability : nc.getCapabilities()) {
                    supported = supported && VCN_SUPPORTED_CAPABILITIES.contains(capability);
                }
                if (supported) nc.addCapability(NET_CAPABILITY_NOT_VCN_MANAGED);
            }
            return new NetworkRequest(nc);
        }

        public Builder addCapability(int capability) {
            mNetworkCapabilities.addCapability(capability);
            if (capability == NET_CAPABILITY_NOT_VCN_MANAGED) mShouldDeduceNotVcnManaged = false;
            return this;
        }

        public Builder removeCapability(int capability) {
            mNetworkCapabilities.removeCapability(capability);
            if (capability == NET_CAPABILITY_NOT_VCN_MANAGED) mShouldDeduceNotVcnManaged = false;
            return this;
        }

        public Builder clearCapabilities() {
            mNetworkCapabilities.clearAll();
            mShouldDeduceNotVcnManaged = false;
            return this;
        }

        public Builder addTransportType(int transportType) {
            mNetworkCapabilities.addTransportType(transportType);
            return this;
        }

        public Builder removeTransportType(int transportType) {
            mNetworkCapabilities.removeTransportType(transportType);
            return this;
        }

        /**
         * As AOSP's: a subscription ID, else an interface's name, but for the test transport's TestNetworkSpecifier,
         * a hidden class no app's network has, for which the name is an Ethernet interface's too.
         */
        @Deprecated
        public Builder setNetworkSpecifier(String networkSpecifier) {
            try {
                int subId = Integer.parseInt(networkSpecifier);
                return setNetworkSpecifier(new TelephonyNetworkSpecifier.Builder().setSubscriptionId(subId).build());
            } catch (NumberFormatException nfe) {
                if (networkSpecifier == null || networkSpecifier.isEmpty()) {
                    return setNetworkSpecifier((NetworkSpecifier) null);
                }
                return setNetworkSpecifier(new EthernetNetworkSpecifier(networkSpecifier));
            }
        }

        public Builder setNetworkSpecifier(NetworkSpecifier networkSpecifier) {
            if (networkSpecifier instanceof MatchAllNetworkSpecifier) {
                throw new IllegalArgumentException("A MatchAllNetworkSpecifier is not permitted");
            }
            mNetworkCapabilities.setNetworkSpecifier(networkSpecifier);
            mShouldDeduceNotVcnManaged = false;
            return this;
        }

        /** As AOSP's: whether networks that apply to other UIDs match, which no network of Minato's does. */
        public Builder setIncludeOtherUidNetworks(boolean include) {
            return this;
        }

        /** As AOSP's but kept nowhere: a request for subscriptions needs NETWORK_FACTORY, which no app has. */
        public Builder setSubscriptionIds(java.util.Set<Integer> subIds) {
            Objects.requireNonNull(subIds);
            return this;
        }
    }
}
