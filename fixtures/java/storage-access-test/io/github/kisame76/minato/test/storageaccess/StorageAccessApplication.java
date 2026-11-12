package io.github.kisame76.minato.test.storageaccess;

import android.app.Application;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import android.util.Log;

/**
 * faccessat and stat on the directories above the app's external storage, as an app uid sees them: /storage and
 * its emulated volumes have no mount of their own in Minato, so the runtime synthesises them as directories and
 * faccessat answers for them. This probe asks android.system.Os.access (faccessat) for F_OK, R_OK,
 * W_OK and X_OK and Os.stat (the mode bits and whether the app owns it) on each, so the AVD's app-uid answers and
 * Minato's can be compared. The output is normalised (yes/no and self-owned, not the raw uid, which differs by
 * install), so the lines compare byte for byte. The launcher activity is the framework's own android.app.Activity, so
 * `am start` starts the process on Android.
 */
public class StorageAccessApplication extends Application {
    static final String TAG = "storage-access-test";

    private static final String[] PATHS = {
        "/storage",
        "/storage/emulated",
        "/storage/emulated/0",
        "/sdcard",
        "/storage/self",
        "/storage/self/primary",
        "/storage/emulated/0/Android",
        "/storage/emulated/0/Android/data",
        "/storage/emulated/0/Android/obb",
    };

    private static final int[] MODES = {OsConstants.F_OK, OsConstants.R_OK, OsConstants.W_OK, OsConstants.X_OK};
    private static final String[] MODE_NAMES = {"F", "R", "W", "X"};

    /** One Os.access result: yes, no (EACCES), or the errno name for any other failure. */
    private static String access(String path, int mode) {
        try {
            return Os.access(path, mode) ? "yes" : "no";
        } catch (ErrnoException e) {
            return OsConstants.errnoName(e.errno);
        }
    }

    /** The mode bits (octal) and whether the app owns the file; the errno name when stat fails. */
    private static String stat(String path) {
        try {
            StructStat st = Os.stat(path);
            return "mode=0" + Integer.toOctalString(st.st_mode) + " self-owned=" + (st.st_uid == Os.getuid());
        } catch (ErrnoException e) {
            return "stat " + OsConstants.errnoName(e.errno);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        for (String path : PATHS) {
            StringBuilder line = new StringBuilder(path).append(": access");
            for (int i = 0; i < MODES.length; i++) {
                line.append(' ').append(MODE_NAMES[i]).append('=').append(access(path, MODES[i]));
            }
            line.append("; ").append(stat(path));
            Log.i(TAG, line.toString());
        }
        Log.i(TAG, "done");
    }
}
