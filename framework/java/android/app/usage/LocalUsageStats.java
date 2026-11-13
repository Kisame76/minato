/*
 * The usage stats service: IUsageStatsManager of system_server, local in the
 * app's process, which ServiceManager's table gives as "usagestats" and AOSP's UsageStatsManager asks. An app that
 * holds no usage access (PACKAGE_USAGE_STATS) gets no statistics from Android's service; Minato keeps none, so every
 * query answers as that: nothing [read: UsageStatsService, android-15.0.0_r1]. Every call is AIDL's Default answer. Not
 * under com.android.minato, whose classes tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.app.usage;

import android.os.Binder;
import android.os.IBinder;

public final class LocalUsageStats extends IUsageStatsManager.Default {
    private final Binder mBinder = new Binder();

    private LocalUsageStats() {
        mBinder.attachInterface(this, IUsageStatsManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "usagestats": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalUsageStats().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }
}
