// The hand file of our StorageManager (ADR-0045 decisions 3 and 4; the storage and app ops step, Godot's stop 4):
// tools/managergen writes the class from the public API, whose methods log once and return their default, as for
// AppOpsManager (a mechanism under decision 3 by 2026-10-04: AOSP's class initializer takes vold's and
// installd's AIDL constants, which the fetch lacks). The members here answer for decision 4's one emulated volume, the
// user's /storage/emulated/<user>, always mounted, as Environment's patch has it (step c1). It is built as Android's
// VolumeInfo.buildStorageVolume builds the primary emulated volume. The hidden members are AOSP's that taken files
// call. ContextImpl gives one per context, as the registry's cached fetcher does.
import android.content.Context;
import android.content.res.Resources;
import android.os.Environment;
import android.os.FileUtils;
import android.os.Looper;
import android.os.UserHandle;
import android.util.Slog;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Objects;
import java.util.UUID;

class StorageManager {
    private static final String TAG = "StorageManager";

    /** @hide As AOSP's. */
    public static final String UUID_PRIVATE_INTERNAL = null;

    /** @hide As AOSP's. */
    public static final String UUID_PRIMARY_PHYSICAL = "primary_physical";

    /** @hide As AOSP's. */
    public static final String UUID_SYSTEM = "system";

    /** As AOSP's: the default internal storage, the same on every device. */
    public static final UUID UUID_DEFAULT = UUID.fromString("41217664-9172-527a-b3d5-edabb50a7d69");

    /** @hide As AOSP's. */
    public static final UUID UUID_PRIMARY_PHYSICAL_ = UUID.fromString("0f95a519-dae7-5abf-9519-fbd6209e05fd");

    /** @hide As AOSP's. */
    public static final UUID UUID_SYSTEM_ = UUID.fromString("5d258386-e60d-59e3-826d-0089cdd42cc0");

    /** @hide As AOSP's: getVolumeList's flags, which the one volume answers alike. */
    public static final int FLAG_FOR_WRITE = 1 << 8;

    /** @hide */
    public static final int FLAG_REAL_STATE = 1 << 9;

    /** @hide */
    public static final int FLAG_INCLUDE_INVISIBLE = 1 << 10;

    /** @hide */
    public static final int FLAG_INCLUDE_RECENT = 1 << 11;

    /** @hide */
    public static final int FLAG_INCLUDE_SHARED_PROFILE = 1 << 12;

    /** @hide As AOSP's: getAllocatableBytes may claim the full reserve, not only the low-storage one. */
    public static final int FLAG_ALLOCATE_AGGRESSIVE = 1 << 0;

    /** As AOSP's convert: the prefix that makes a FAT volume's identifier a UUID. */
    private static final String FAT_UUID_PREFIX = "fafafafa-fafa-5afa-8afa-fafa";

    private final Context mContext;

    /** @hide AOSP's signature, which ContextImpl calls; there is no storage service to look up. */
    public StorageManager(Context context, Looper looper) {
        mContext = context;
    }

    public List<StorageVolume> getStorageVolumes() {
        return new ArrayList<>(Arrays.asList(
                getVolumeList(mContext.getUserId(), FLAG_REAL_STATE | FLAG_INCLUDE_INVISIBLE)));
    }

    /** The one volume, as Android lists no recent volume beside it. */
    public List<StorageVolume> getRecentStorageVolumes() {
        return getStorageVolumes();
    }

    public StorageVolume getPrimaryStorageVolume() {
        return getVolumeList(mContext.getUserId(), FLAG_REAL_STATE | FLAG_INCLUDE_INVISIBLE)[0];
    }

    public StorageVolume getStorageVolume(File file) {
        return getStorageVolume(getVolumeList(), file);
    }

    /** @hide */
    public StorageVolume[] getVolumeList() {
        return getVolumeList(mContext.getUserId(), 0);
    }

    /** @hide The user's one emulated volume, whatever the flags ask: it is always mounted and visible. */
    public static StorageVolume[] getVolumeList(int userId, int flags) {
        return new StorageVolume[] {primaryEmulatedVolume(userId)};
    }

    /** @hide */
    public static StorageVolume getStorageVolume(File file, int userId) {
        return getStorageVolume(getVolumeList(userId, 0), file);
    }

    /** As AOSP's: the volume a path lies on, by canonical paths. Minato has no /mnt/content/ paths to translate. */
    private static StorageVolume getStorageVolume(StorageVolume[] volumes, File file) {
        if (file == null) {
            return null;
        }
        try {
            file = file.getCanonicalFile();
        } catch (IOException ignored) {
            Slog.d(TAG, "Could not get canonical path for " + file);
            return null;
        }
        for (StorageVolume volume : volumes) {
            File volumeFile = volume.getPathFile();
            try {
                volumeFile = volumeFile.getCanonicalFile();
            } catch (IOException ignored) {
                continue;
            }
            if (FileUtils.contains(volumeFile, file)) {
                return volume;
            }
        }
        return null;
    }

    /** @hide As AOSP's. */
    public static UUID convert(String uuid) {
        // UUID_PRIVATE_INTERNAL is null, so this accepts null.
        if (Objects.equals(uuid, UUID_PRIVATE_INTERNAL)) {
            return UUID_DEFAULT;
        } else if (Objects.equals(uuid, UUID_PRIMARY_PHYSICAL)) {
            return UUID_PRIMARY_PHYSICAL_;
        } else if (Objects.equals(uuid, UUID_SYSTEM)) {
            return UUID_SYSTEM_;
        } else if (uuid.length() == 9 && uuid.charAt(4) == '-') {
            // A FAT volume's 32-bit identifier, XXXX-YYYY, as the UUID fafafafa-fafa-5afa-8afa-fafaXXXXYYYY.
            return UUID.fromString(FAT_UUID_PREFIX + uuid.replace("-", ""));
        } else {
            return UUID.fromString(uuid);
        }
    }

    /** @hide As AOSP's. */
    public static String convert(UUID storageUuid) {
        if (UUID_DEFAULT.equals(storageUuid)) {
            return UUID_PRIVATE_INTERNAL;
        } else if (UUID_PRIMARY_PHYSICAL_.equals(storageUuid)) {
            return UUID_PRIMARY_PHYSICAL;
        } else if (UUID_SYSTEM_.equals(storageUuid)) {
            return UUID_SYSTEM;
        } else {
            String uuidString = storageUuid.toString();
            if (uuidString.startsWith(FAT_UUID_PREFIX)) {
                String fatStr = uuidString.substring(FAT_UUID_PREFIX.length()).toUpperCase(Locale.US);
                return fatStr.substring(0, 4) + "-" + fatStr.substring(4);
            }
            return storageUuid.toString();
        }
    }

    /**
     * As AOSP's: the storage UUID of the volume a path lies on -- UUID_DEFAULT for internal storage (/data) and for
     * the one emulated volume (decision 4). Throws, as AOSP does, when the path is on no storage volume. Without this
     * a game that checks free space with StorageManager.getAllocatableBytes(getUuidForPath(dir)) gets null and then 0.
     */
    public UUID getUuidForPath(File path) throws IOException {
        Objects.requireNonNull(path);
        // Match on the absolute path and the emulated volume's known symlink aliases (/sdcard and
        // /storage/self/primary both lead to /storage/emulated/<user>). getCanonicalFile is not used to resolve them:
        // this runtime's canonicalization does not follow symlinks, so a path reached through one would be missed.
        final File file = path.getAbsoluteFile();
        if (FileUtils.contains(Environment.getDataDirectory(), file)                 // internal storage, /data
                || getStorageVolume(getVolumeList(), file) != null                  // the emulated volume
                || FileUtils.contains(new File("/sdcard"), file)                    // its aliases
                || FileUtils.contains(new File("/storage/self/primary"), file)) {
            return UUID_DEFAULT;
        }
        throw new IOException("Failed to find a storage device for " + path);
    }

    /**
     * As AOSP's StorageManagerService.getAllocatableBytes: the bytes an app may allocate now -- the usable space less
     * the low-storage reserve getStorageLowBytes (min(5% of total, 500 MiB)), or less getStorageFullBytes (1 MiB) with
     * FLAG_ALLOCATE_AGGRESSIVE. We support no cache quota, so no reclaimable cache is added (AOSP's cacheClearable is 0
     * for us). A stub returning 0 made a Unity 6 title report "Not enough storage space" though the device was nearly
     * empty.
     */
    public long getAllocatableBytes(UUID storageUuid) throws IOException {
        return getAllocatableBytes(storageUuid, 0);
    }

    /** @hide */
    public long getAllocatableBytes(UUID storageUuid, int flags) throws IOException {
        final File path = pathForUuid(storageUuid);
        final long reserved = (flags & FLAG_ALLOCATE_AGGRESSIVE) != 0 ? getStorageFullBytes(path)
                                                                      : getStorageLowBytes(path);
        return Math.max(0, path.getUsableSpace() - reserved);
    }

    /**
     * As AOSP's: succeeds when the volume already holds the bytes, otherwise fails like AOSP. We reserve nothing -- the
     * file system has the space and the app writes as usual -- so this only reports whether the request fits.
     */
    public void allocateBytes(UUID storageUuid, long bytes) throws IOException {
        final long allocatable = getAllocatableBytes(storageUuid, 0);
        if (bytes > allocatable) {
            throw new IOException("Failed to allocate " + bytes + " bytes: only " + allocatable + " allocatable");
        }
    }

    public void allocateBytes(FileDescriptor fd, long bytes) throws IOException {
        allocateBytes(UUID_DEFAULT, bytes);
    }

    /** @hide As AOSP's: the bytes free until the path counts as low on storage, min(5% of total, 500 MiB). */
    public long getStorageLowBytes(File path) {
        final long lowBytes = (path.getTotalSpace() * 5) / 100;  // SYS_STORAGE_THRESHOLD_PERCENTAGE default
        return Math.min(lowBytes, 500L * 1024 * 1024);           // DEFAULT_THRESHOLD_MAX_BYTES
    }

    /** @hide As AOSP's: the bytes free at which the path counts as full, 1 MiB. */
    public long getStorageFullBytes(File path) {
        return 1L * 1024 * 1024;  // DEFAULT_FULL_THRESHOLD_BYTES
    }

    /** @hide The volume path a storage UUID names: internal for UUID_DEFAULT, the emulated volume for the primary
     *  physical. Static and public so StorageStatsManager answers free space from the same place (ADR-0045). */
    public static File pathForUuid(UUID storageUuid) throws IOException {
        if (UUID_DEFAULT.equals(storageUuid)) {
            return Environment.getDataDirectory();
        } else if (UUID_PRIMARY_PHYSICAL_.equals(storageUuid)) {
            return Environment.getExternalStorageDirectory();
        }
        throw new IOException("Failed to find a storage device for " + storageUuid);
    }

    /** @hide As AOSP's: app and system see the same shared storage paths. */
    public File translateAppToSystem(File file, int pid, int uid) {
        return file;
    }

    /** @hide As AOSP's. */
    public File translateSystemToApp(File file, int pid, int uid) {
        return file;
    }

    /**
     * The user's primary emulated volume as VolumeInfo.buildStorageVolume builds it on Android: its internal path its
     * path, as getInternalPathForUser gives an emulated volume; the internal private volume's description and UUID, no
     * file system UUID, primary and so not removable, not externally managed, without mass storage or a file size
     * limit, owned by the user, mounted.
     */
    private static StorageVolume primaryEmulatedVolume(int userId) {
        return new StorageVolume("emulated;" + userId, new File("/storage/emulated/" + userId),
                new File("/storage/emulated/" + userId),
                Resources.getSystem().getString(com.android.internal.R.string.storage_internal), true, false, true,
                false, false, 0, UserHandle.of(userId), UUID_DEFAULT, null, Environment.MEDIA_MOUNTED);
    }
}
