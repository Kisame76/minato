// The written part of NetworkInfo (ADR-0054 step d)), which tools/managergen joins with the API's methods: a legacy
// network's type, state and flags as AOSP's (connectivity's NetworkInfo.java at android-15.0.0_r1) keeps them, with its
// map from detailed states to states and the hidden setters ConnectivityManager uses (LocalNetwork).
import java.util.EnumMap;

class NetworkInfo {
    /** As AOSP's stateMap: the coarse state of each detailed one. */
    private static final EnumMap<DetailedState, State> STATES = new EnumMap<>(DetailedState.class);
    static {
        STATES.put(DetailedState.IDLE, State.DISCONNECTED);
        STATES.put(DetailedState.SCANNING, State.DISCONNECTED);
        STATES.put(DetailedState.CONNECTING, State.CONNECTING);
        STATES.put(DetailedState.AUTHENTICATING, State.CONNECTING);
        STATES.put(DetailedState.OBTAINING_IPADDR, State.CONNECTING);
        STATES.put(DetailedState.VERIFYING_POOR_LINK, State.CONNECTING);
        STATES.put(DetailedState.CAPTIVE_PORTAL_CHECK, State.CONNECTING);
        STATES.put(DetailedState.CONNECTED, State.CONNECTED);
        STATES.put(DetailedState.SUSPENDED, State.SUSPENDED);
        STATES.put(DetailedState.DISCONNECTING, State.DISCONNECTING);
        STATES.put(DetailedState.DISCONNECTED, State.DISCONNECTED);
        STATES.put(DetailedState.FAILED, State.DISCONNECTED);
        STATES.put(DetailedState.BLOCKED, State.DISCONNECTED);
    }

    private final int mNetworkType;
    private final int mSubtype;
    private final String mTypeName;
    private final String mSubtypeName;
    private State mState;
    private DetailedState mDetailedState;
    private String mReason;
    private String mExtraInfo;
    private boolean mIsFailover;
    private boolean mIsAvailable;
    private boolean mIsRoaming;

    /** ConnectivityManager's hidden TYPE_NONE. */
    private static final int TYPE_NONE = -1;

    /** As AOSP's: a valid legacy type or TYPE_NONE, idle in an unknown state. */
    @Deprecated
    public NetworkInfo(int type, int subtype, String typeName, String subtypeName) {
        if (!ConnectivityManager.isNetworkTypeValid(type) && type != TYPE_NONE) {
            throw new IllegalArgumentException("Invalid network type: " + type);
        }
        mNetworkType = type;
        mSubtype = subtype;
        mTypeName = typeName;
        mSubtypeName = subtypeName;
        setDetailedState(DetailedState.IDLE, null, null);
        mState = State.UNKNOWN;
    }

    @Deprecated public int getType() { synchronized (this) { return mNetworkType; } }
    @Deprecated public int getSubtype() { synchronized (this) { return mSubtype; } }
    @Deprecated public String getTypeName() { synchronized (this) { return mTypeName; } }
    @Deprecated public String getSubtypeName() { synchronized (this) { return mSubtypeName; } }

    @Deprecated
    public boolean isConnectedOrConnecting() {
        synchronized (this) {
            return mState == State.CONNECTED || mState == State.CONNECTING;
        }
    }

    @Deprecated public boolean isConnected() { synchronized (this) { return mState == State.CONNECTED; } }
    @Deprecated public boolean isAvailable() { synchronized (this) { return mIsAvailable; } }
    @Deprecated public boolean isFailover() { synchronized (this) { return mIsFailover; } }
    @Deprecated public boolean isRoaming() { synchronized (this) { return mIsRoaming; } }
    @Deprecated public State getState() { synchronized (this) { return mState; } }
    @Deprecated public DetailedState getDetailedState() { synchronized (this) { return mDetailedState; } }
    @Deprecated public String getReason() { synchronized (this) { return mReason; } }
    @Deprecated public String getExtraInfo() { synchronized (this) { return mExtraInfo; } }

    /** @hide As AOSP's. */
    @Deprecated
    public void setIsAvailable(boolean isAvailable) {
        synchronized (this) {
            mIsAvailable = isAvailable;
        }
    }

    /** @hide As AOSP's. */
    @Deprecated
    public void setRoaming(boolean isRoaming) {
        synchronized (this) {
            mIsRoaming = isRoaming;
        }
    }

    @Deprecated
    public void setDetailedState(DetailedState detailedState, String reason, String extraInfo) {
        synchronized (this) {
            mDetailedState = detailedState;
            mState = STATES.get(detailedState);
            mReason = reason;
            mExtraInfo = extraInfo;
        }
    }

    @Deprecated
    public int describeContents() {
        return 0;
    }

    /** @hide As AOSP's. */
    @Override
    public String toString() {
        synchronized (this) {
            return "[type: " + mTypeName + "[" + mSubtypeName + "], state: " + mState + "/" + mDetailedState
                    + ", reason: " + (mReason == null ? "(unspecified)" : mReason) + ", extra: "
                    + (mExtraInfo == null ? "(none)" : mExtraInfo) + ", failover: " + mIsFailover + ", available: "
                    + mIsAvailable + ", roaming: " + mIsRoaming + "]";
        }
    }
}
