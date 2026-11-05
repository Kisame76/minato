package io.github.kisame76.minato.test.storagespace;

import android.app.Application;
import android.app.usage.StorageStats;
import android.app.usage.StorageStatsManager;
import android.os.Environment;
import android.os.Process;
import android.os.StatFs;
import android.os.storage.StorageManager;
import android.system.Os;
import android.system.StructStatVfs;
import android.util.Log;

import java.io.File;
import java.util.UUID;

/**
 * Why a Unity 6 title reports "Not enough storage space to install required resources." although the device has tens
 * of GB free: for every folder a resource extractor checks, this prints the free-space answers of every API -- StatFs,
 * java.io.File, StorageManager.getAllocatableBytes and the raw statvfs. The root cause was StorageManager returning 0
 * allocatable bytes and a null UUID (managergen stubs). It logs two kinds of line:
 *  - "storage-space-test: ... relations: ..." -- normalized relations (available>0, allocatable<=usable, uuid==default,
 *    ...) that hold on any device with free space, so they compare against the AVD's (fixtures/expected).
 *  - "storage-space-test-device: ... raw: ..." -- the actual byte counts, which differ by device, so java_app_run.py
 *    only shows them (for reading the shape across the Mac, the Simulator, the iPhone and the AVD).
 */
public class StorageSpaceApplication extends Application {
    static final String TAG = "storage-space-test";
    static final String DEVICE = "storage-space-test-device";

    private interface LongCall {
        long get() throws Throwable;
    }

    private static final long UNAVAILABLE = Long.MIN_VALUE;

    /** The call's value, or UNAVAILABLE if it throws (the raw line names the exception). */
    private static long value(LongCall body) {
        try {
            return body.get();
        } catch (Throwable t) {
            return UNAVAILABLE;
        }
    }

    private static String outcome(long v) {
        return v == UNAVAILABLE ? "exc" : v > 0 ? "pos" : v == 0 ? "zero" : "neg";
    }

    private static String show(long v) {
        return v == UNAVAILABLE ? "exc" : v + " (" + String.format("%.2f", v / (1024.0 * 1024 * 1024)) + " GiB)";
    }

    /** Slack for the alloc>=usable-low relation: getUsableSpace and getAllocatableBytes are two separate reads of the
     * filesystem, so a free-space change between them (a busy disk) can make allocatable fall a little below usable-low
     * although AOSP computes the two equal from one snapshot. 256 MiB absorbs that drift without masking a real
     * shortfall. */
    private static final long RELATION_SLACK = 256L * 1024 * 1024;

    /** The low-storage reserve AOSP's getAllocatableBytes subtracts: min(5% of total, 500 MiB). */
    private static long lowReserve(long total) {
        return total <= 0 ? UNAVAILABLE : Math.min((total * 5) / 100, 500L * 1024 * 1024);
    }

    private void dump(String label, File path) {
        if (path == null) {
            Log.i(TAG, label + " = null");
            return;
        }
        final String name = path.getPath();
        final StorageManager sm = getSystemService(StorageManager.class);
        final long statfsAvail = value(() -> new StatFs(name).getAvailableBytes());
        final long statfsTotal = value(() -> new StatFs(name).getTotalBytes());
        final long usable = value(path::getUsableSpace);
        final long freeSpace = value(path::getFreeSpace);
        final long total = value(path::getTotalSpace);
        final long allocatable = value(() -> sm.getAllocatableBytes(sm.getUuidForPath(path)));
        final UUID uuid = uuidOf(sm, path);
        final long statvfsAvail = value(() -> { StructStatVfs s = Os.statvfs(name); return s.f_bavail * s.f_frsize; });
        final long low = lowReserve(total);

        Log.i(TAG, label + " = " + name);
        Log.i(TAG, label + " relations: statfs-avail=" + outcome(statfsAvail) + " file-usable=" + outcome(usable)
                + " statvfs-avail=" + outcome(statvfsAvail)
                + " allocatable=" + outcome(allocatable)
                + " alloc<=usable=" + yesNo(allocatable != UNAVAILABLE && usable != UNAVAILABLE && allocatable <= usable)
                + " alloc>=usable-low=" + yesNo(allocatable != UNAVAILABLE && usable != UNAVAILABLE && low != UNAVAILABLE
                        && allocatable >= usable - low - RELATION_SLACK)
                + " uuid=" + (uuid == null ? "null" : StorageManager.UUID_DEFAULT.equals(uuid) ? "default" : "other"));
        Log.i(DEVICE, label + " raw: statfs(avail=" + show(statfsAvail) + " total=" + show(statfsTotal) + ") file(usable="
                + show(usable) + " free=" + show(freeSpace) + " total=" + show(total) + ") allocatable=" + show(allocatable)
                + " usable-low=" + (usable == UNAVAILABLE || low == UNAVAILABLE ? "exc" : show(usable - low))
                + " statvfs-avail=" + show(statvfsAvail) + " uuid=" + uuid);
    }

    private static UUID uuidOf(StorageManager sm, File path) {
        try {
            return sm.getUuidForPath(path);
        } catch (Throwable t) {
            return null;
        }
    }

    private static String yesNo(boolean b) {
        return b ? "yes" : "no";
    }

    private static String notNeg(long v) {
        return v == UNAVAILABLE ? "exc" : v >= 0 ? "yes" : "no";
    }

    /** StorageStatsManager, which engines also use to read free space: free/total bytes and the app's own stats. */
    private void dumpStats() {
        final StorageStatsManager sm = getSystemService(StorageStatsManager.class);
        if (sm == null) {
            Log.i(TAG, "storageStats: null service");
            return;
        }
        final long free = value(() -> sm.getFreeBytes(StorageManager.UUID_DEFAULT));
        final long total = value(() -> sm.getTotalBytes(StorageManager.UUID_DEFAULT));
        long app = UNAVAILABLE, data = UNAVAILABLE, cache = UNAVAILABLE;
        try {
            final StorageStats s = sm.queryStatsForPackage(StorageManager.UUID_DEFAULT, getPackageName(),
                    Process.myUserHandle());
            app = s.getAppBytes();
            data = s.getDataBytes();
            cache = s.getCacheBytes();
        } catch (Throwable ignored) {
        }
        Log.i(TAG, "storageStats relations: free=" + outcome(free) + " total=" + outcome(total)
                + " total>=free=" + yesNo(total != UNAVAILABLE && free != UNAVAILABLE && total >= free)
                + " pkg-app=" + notNeg(app) + " pkg-data=" + notNeg(data) + " pkg-cache=" + notNeg(cache));
        Log.i(DEVICE, "storageStats raw: free=" + show(free) + " total=" + show(total) + " pkg(app=" + show(app)
                + " data=" + show(data) + " cache=" + show(cache) + ")");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        dump("filesDir", getFilesDir());
        dump("cacheDir", getCacheDir());
        dump("externalFilesDir", getExternalFilesDir(null));
        dump("obbDir", getObbDir());
        dump("externalStorageDir", Environment.getExternalStorageDirectory());  // /storage/emulated/0
        dump("sdcard", new File("/sdcard"));
        dump("dataDir", Environment.getDataDirectory());  // /data
        dumpStats();
        Log.i(TAG, "done");
    }
}
