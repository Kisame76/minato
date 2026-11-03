// The written part of WifiManager (ADR-0045 decision 3), which tools/managergen joins with the API's methods: the
// hidden ctor SystemServiceRegistry calls, and decision 4's answers as changed on 2026-10-04 and by ADR-0054 step d):
// the iPhone has Wi-Fi, and PackageManager reports the feature, but iOS gives an app no Wi-Fi API without entitlements,
// so Wi-Fi is enabled while the host's network goes over it (LocalNetwork) and disabled otherwise, and the connection
// info is what Android shows an app without a location permission. The multicast and Wi-Fi locks count as Android's do
// (packages/modules/Wifi's WifiManager.java:7822-8001 and 8230-8380 at android-15.0.0_r1), with the service's
// permission and lock mode checks (WifiServiceImpl.java:1172-1176, 5613-5632, 5658-5664, 5679-5690) and the limit of
// 50 locks, but nothing behind them.
import android.content.Context;
import com.android.internal.minato.LocalNetwork;

class WifiManager {
    /** As AOSP's: the most locks one WifiManager (one per context) holds at once, multicast and Wi-Fi locks alike. */
    private static final int MAX_ACTIVE_LOCKS = 50;
    /** Manifest.permission's names, written out: tools/managers-check.sh compiles this class without android.R. */
    private static final String ACCESS_WIFI_STATE = "android.permission.ACCESS_WIFI_STATE";
    private static final String CHANGE_WIFI_MULTICAST_STATE = "android.permission.CHANGE_WIFI_MULTICAST_STATE";
    private static final String WAKE_LOCK = "android.permission.WAKE_LOCK";

    private final Context context;
    private int mActiveLockCount;

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's WifiManager takes its
     * IWifiManager too; ours has none.
     */
    public WifiManager(Context context) {
        this.context = context;
    }

    public boolean isWifiEnabled() {
        return getWifiState() == WIFI_STATE_ENABLED;
    }

    /** As WifiServiceImpl.getWifiEnabledState (WifiServiceImpl.java:1136-1139, 1543-1544): ACCESS_WIFI_STATE first. */
    public int getWifiState() {
        context.enforceCallingOrSelfPermission(ACCESS_WIFI_STATE, "WifiService");
        return LocalNetwork.wifi() ? WIFI_STATE_ENABLED : WIFI_STATE_DISABLED;
    }

    /** As WifiServiceImpl.getConnectionInfo: ACCESS_WIFI_STATE first, then the redacted connection. */
    public WifiInfo getConnectionInfo() {
        context.enforceCallingOrSelfPermission(ACCESS_WIFI_STATE, "WifiService");
        return LocalNetwork.wifiInfo();
    }

    public MulticastLock createMulticastLock(String tag) {
        return new MulticastLock(tag);
    }

    public WifiLock createWifiLock(int lockType, String tag) {
        return new WifiLock(lockType, tag);
    }

    @Deprecated
    public WifiLock createWifiLock(String tag) {
        return new WifiLock(WIFI_MODE_FULL, tag);
    }

    /** AOSP's lock's side of an acquire, after the service's checks: the limit, then the count. */
    private void countLock() {
        synchronized (this) {
            if (mActiveLockCount >= MAX_ACTIVE_LOCKS) {
                throw new UnsupportedOperationException("Exceeded maximum number of wifi locks");
            }
            mActiveLockCount++;
        }
    }

    private void uncountLock() {
        synchronized (this) {
            mActiveLockCount--;
        }
    }

    public class MulticastLock {
        private final String mTag;
        private int mRefCount;
        private boolean mRefCounted = true;
        private boolean mHeld;

        private MulticastLock(String tag) {
            mTag = tag;
        }

        public void acquire() {
            synchronized (this) {
                if (mRefCounted ? (++mRefCount == 1) : (!mHeld)) {
                    context.enforceCallingOrSelfPermission(CHANGE_WIFI_MULTICAST_STATE, "WifiService");
                    countLock();
                    mHeld = true;
                }
            }
        }

        public void release() {
            synchronized (this) {
                if (mRefCounted ? (--mRefCount == 0) : (mHeld)) {
                    context.enforceCallingOrSelfPermission(CHANGE_WIFI_MULTICAST_STATE, "WifiService");
                    uncountLock();
                    mHeld = false;
                }
                if (mRefCount < 0) {
                    throw new RuntimeException("MulticastLock under-locked " + mTag);
                }
            }
        }

        public void setReferenceCounted(boolean refCounted) {
            mRefCounted = refCounted;
        }

        public boolean isHeld() {
            synchronized (this) {
                return mHeld;
            }
        }

        /** @hide As AOSP's: a lock that is collected while held is released. */
        @Override
        protected void finalize() throws Throwable {
            super.finalize();
            setReferenceCounted(false);
            release();
        }
    }

    public class WifiLock {
        private final String mTag;
        private final int mLockType;
        private int mRefCount;
        private boolean mRefCounted = true;
        private boolean mHeld;

        private WifiLock(int lockType, String tag) {
            mTag = tag;
            mLockType = lockType;
        }

        public void acquire() {
            synchronized (this) {
                if (mRefCounted ? (++mRefCount == 1) : (!mHeld)) {
                    context.enforceCallingOrSelfPermission(WAKE_LOCK, null);
                    // WifiLockManager.isValidLockMode (WifiLockManager.java:467-475).
                    if (mLockType != WIFI_MODE_FULL && mLockType != WIFI_MODE_SCAN_ONLY
                            && mLockType != WIFI_MODE_FULL_HIGH_PERF && mLockType != WIFI_MODE_FULL_LOW_LATENCY) {
                        throw new IllegalArgumentException("lockMode =" + mLockType);
                    }
                    countLock();
                    mHeld = true;
                }
            }
        }

        public void release() {
            synchronized (this) {
                if (mRefCounted ? (--mRefCount == 0) : (mHeld)) {
                    context.enforceCallingOrSelfPermission(WAKE_LOCK, null);
                    uncountLock();
                    mHeld = false;
                }
                if (mRefCount < 0) {
                    throw new RuntimeException("WifiLock under-locked " + mTag);
                }
            }
        }

        public void setReferenceCounted(boolean refCounted) {
            mRefCounted = refCounted;
        }

        public boolean isHeld() {
            synchronized (this) {
                return mHeld;
            }
        }

        /** @hide As AOSP's: a lock that is collected while held is released. */
        @Override
        protected void finalize() throws Throwable {
            super.finalize();
            synchronized (this) {
                if (mHeld) {
                    context.enforceCallingOrSelfPermission(WAKE_LOCK, null);
                    uncountLock();
                }
            }
        }
    }
}
