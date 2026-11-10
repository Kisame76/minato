// The written part of SystemHealthManager (services bundle A of the system services gap): the battery statistics of an
// app's own uid. Minato keeps none, so the snapshot is an empty UidHealthStats, which is what the emulator
// (apk-runner-35-bare) gives an app for its own uid: a HealthStats whose data type is UidHealthStats
// [verified: services-f-test]. Another uid needs a permission an app does not hold: SecurityException
// [inferred from android-15.0.0_r1, SystemHealthManager.takeUidSnapshot's service call].
import android.os.Parcel;
import android.os.Process;

class SystemHealthManager {
    /** @hide As the tag's, which SystemServiceRegistry calls. */
    public SystemHealthManager() {
    }

    public HealthStats takeMyUidSnapshot() {
        return emptyUidSnapshot();
    }

    public HealthStats takeUidSnapshot(int uid) {
        if (uid != Process.myUid()) {
            throw new SecurityException("Neither user " + Process.myUid()
                    + " nor current process has android.permission.BATTERY_STATS.");
        }
        return emptyUidSnapshot();
    }

    public HealthStats[] takeUidSnapshots(int[] uids) {
        final HealthStats[] result = new HealthStats[uids.length];
        for (int i = 0; i < uids.length; i++) result[i] = takeUidSnapshot(uids[i]);
        return result;
    }

    private static HealthStats emptyUidSnapshot() {
        final HealthStatsWriter writer = new HealthStatsWriter(UidHealthStats.CONSTANTS);
        final Parcel parcel = Parcel.obtain();
        try {
            writer.flattenToParcel(parcel);
            parcel.setDataPosition(0);
            return new HealthStats(parcel);
        } finally {
            parcel.recycle();
        }
    }
}
