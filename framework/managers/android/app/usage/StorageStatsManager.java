// The hand file of our StorageStatsManager (ADR-0045): the other way an app reads free space, which some engines use
// instead of StorageManager.getAllocatableBytes. It answers from the same place -- StorageManager.pathForUuid -- so
// one volume's numbers are consistent: getFreeBytes is the volume's usable space, getTotalBytes the total rounded as
// FileUtils.roundStorageSize does (the marketing size the AVD reports, e.g. 8 GB), and queryStats* give the app's own
// cheap sizes (its APK as the code), Android's defaults otherwise. Registered for Context.STORAGE_STATS_SERVICE. A
// Unity 6 title reported "Not enough storage space" when this and getAllocatableBytes returned 0 (managergen stubs).
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.UserHandle;
import android.os.storage.StorageManager;

import java.io.File;
import java.io.IOException;
import java.util.UUID;

class StorageStatsManager {
    private final Context mContext;

    /** @hide The signature the service registry constructs; there is no storage-stats service to look up. */
    public StorageStatsManager(Context context) {
        mContext = context;
    }

    public long getFreeBytes(UUID storageUuid) throws IOException {
        return StorageManager.pathForUuid(storageUuid).getUsableSpace();
    }

    public long getTotalBytes(UUID storageUuid) throws IOException {
        return roundStorageSize(StorageManager.pathForUuid(storageUuid).getTotalSpace());
    }

    public StorageStats queryStatsForPackage(UUID storageUuid, String packageName, UserHandle user)
            throws IOException, PackageManager.NameNotFoundException {
        return appStats();
    }

    public StorageStats queryStatsForUid(UUID storageUuid, int uid) throws IOException {
        return appStats();
    }

    public StorageStats queryStatsForUser(UUID storageUuid, UserHandle user) throws IOException {
        return appStats();
    }

    /** The app's own sizes, cheaply: its APK as the code bytes, no counted data or cache (Android's default when the
     *  real figures are unknown). The process holds the one package, so every query answers for it. StorageStats is
     *  AOSP's value type (framework/sources.txt), with a no-arg constructor and public fields the service also
     *  fills. */
    private StorageStats appStats() {
        long code = 0;
        final String source = mContext.getApplicationInfo().sourceDir;
        if (source != null) {
            code = new File(source).length();
        }
        final StorageStats stats = new StorageStats();
        stats.codeBytes = code;
        return stats;
    }

    /**
     * As FileUtils.roundStorageSize: the next of 1,2,4,...,512 times 1000^n at or above `size`, the marketing total.
     */
    private static long roundStorageSize(long size) {
        long val = 1;
        long pow = 1;
        while (val * pow < size) {
            val <<= 1;
            if (val > 512) {
                val = 1;
                pow *= 1000;
            }
        }
        return val * pow;
    }
}
