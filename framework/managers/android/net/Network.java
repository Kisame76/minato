// The written part of Network (ADR-0054 step d)), which tools/managergen joins with the API's methods: a network by
// its netId, equal by it, with AOSP's hidden ctor and its handle (connectivity's Network.java at android-15.0.0_r1).
class Network {
    /** @hide As AOSP's: the network's ID, which ConnectivityManager gives it (LocalNetwork). */
    public final int netId;

    /** @hide As AOSP's. */
    public Network(int netId) {
        this.netId = netId;
    }

    /** As AOSP's: the netId and a magic word, so that a handle is never taken for a netId. */
    public long getNetworkHandle() {
        return netId == 0 ? 0L : ((long) netId << 32) | 0xcafed00dL;
    }

    public int describeContents() {
        return 0;
    }

    /** @hide As AOSP's. */
    @Override
    public boolean equals(Object other) {
        return other instanceof Network && ((Network) other).netId == netId;
    }

    /** @hide As AOSP's. */
    @Override
    public int hashCode() {
        return netId * 11;
    }

    /** @hide As AOSP's. */
    @Override
    public String toString() {
        return Integer.toString(netId);
    }
}
