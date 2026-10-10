// The hand file of our AppOpsManager (ADR-0039 step c2c1, ADR-0045 decisions 3 and 6): tools/managergen writes the
// class from the public API, whose methods log once and return their default, and the members here are the hidden
// ones that taken AOSP files call, and the checks and notes (the storage and app ops step, Godot's stop 6), which
// answer as Android 15 does under decision 6 [verified: services-test on apk-runner-35-bare]:
//   - a runtime permission's op answers MODE_ALLOWED when decision 6 grants the permission and MODE_IGNORED when it
//     does not, whether the app requests the permission or not (CAMERA requested and RECORD_AUDIO not, both
//     MODE_IGNORED on the AVD);
//   - every other op answers its default mode, from AOSP's table, which tools/appopsgen.py writes (AppOpsTable).
// An op answers through its switch op, as AppOpsService does. Notes are not kept: nothing in the process reads them.
// ContextImpl gives one per context. No permission has an op code for ContentProvider (permissionToOpCode), which so
// notes none (step c2c1).
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Parcel;
import android.os.Process;

import java.util.HashMap;
import java.util.Map;

class AppOpsManager {
    /** @hide No app op, as for a permission without one. */
    public static final int OP_NONE = -1;

    /** @hide As AOSP's: the ops Settings' write checks ask by number (gap 13a); appopsgen checks them. */
    public static final int OP_WRITE_SETTINGS = 23;

    /** @hide */
    public static final int OP_SYSTEM_ALERT_WINDOW = 24;

    /** @hide As AOSP's: the op the legacy Camera watches for its shutter sound (the Unity test APK probe's batch C). */
    public static final int OP_PLAY_AUDIO = 28;

    /** @hide As AOSP's: the ops Environment's legacy storage checks ask by number (step c1). */
    public static final int OP_LEGACY_STORAGE = 87;

    /** @hide */
    public static final int OP_MANAGE_EXTERNAL_STORAGE = 92;

    /** @hide */
    public static final int OP_NO_ISOLATED_STORAGE = 99;

    /** The ops by name, as AOSP's sOpStrToOp. */
    private static final Map<String, Integer> sOpStrToOp = new HashMap<>();

    /** Whether an op is a runtime permission's. */
    private static final boolean[] sRuntimePermissionOp = new boolean[AppOpsTable.NAMES.length];

    static {
        for (int op = 0; op < AppOpsTable.NAMES.length; op++) {
            sOpStrToOp.put(AppOpsTable.NAMES[op], op);
        }
        for (int op : AppOpsTable.RUNTIME_PERMISSION_OPS) {
            sRuntimePermissionOp[op] = true;
        }
    }

    private final Context mContext;

    /** @hide ContextImpl's, for its context. */
    public AppOpsManager(Context context) {
        mContext = context;
    }

    /** @hide No permission has an app op on Minato, so ContentProvider notes none. */
    public static int permissionToOpCode(String permission) {
        return OP_NONE;
    }

    /** @hide As AOSP's: the op's name. */
    public static String opToPublicName(int op) {
        return AppOpsTable.NAMES[op];
    }

    /** @hide As AOSP's: the op of a name. */
    public static int strOpToOp(String op) {
        Integer value = sOpStrToOp.get(op);
        if (value == null) {
            throw new IllegalArgumentException("Unknown operation string: " + op);
        }
        return value;
    }

    /** @hide */
    public int checkOpNoThrow(int op, int uid, String packageName) {
        return mode(op, uid);
    }

    /** @hide */
    public int noteOpNoThrow(int op, int uid, String packageName) {
        return mode(op, uid);
    }

    /** @hide As AOSP's: with the attribution tag and message, which a note would record. */
    public int noteOpNoThrow(int op, int uid, String packageName, String attributionTag, String message) {
        return mode(op, uid);
    }

    /** @hide As AOSP's: an errored op throws. */
    public int checkOp(int op, int uid, String packageName) {
        return unlessErrored(op, uid, packageName, mode(op, uid));
    }

    /** @hide */
    public int noteOp(int op, int uid, String packageName) {
        return unlessErrored(op, uid, packageName, mode(op, uid));
    }

    public int unsafeCheckOp(String op, int uid, String packageName) {
        return checkOp(strOpToOp(op), uid, packageName);
    }

    public int unsafeCheckOpNoThrow(String op, int uid, String packageName) {
        return checkOpNoThrow(strOpToOp(op), uid, packageName);
    }

    @Deprecated
    public int checkOp(String op, int uid, String packageName) {
        return checkOp(strOpToOp(op), uid, packageName);
    }

    @Deprecated
    public int checkOpNoThrow(String op, int uid, String packageName) {
        return checkOpNoThrow(strOpToOp(op), uid, packageName);
    }

    @Deprecated
    public int noteOp(String op, int uid, String packageName) {
        return noteOp(strOpToOp(op), uid, packageName);
    }

    public int noteOp(String op, int uid, String packageName, String attributionTag, String message) {
        return noteOp(strOpToOp(op), uid, packageName);
    }

    @Deprecated
    public int noteOpNoThrow(String op, int uid, String packageName) {
        return noteOpNoThrow(strOpToOp(op), uid, packageName);
    }

    public int noteOpNoThrow(String op, int uid, String packageName, String attributionTag, String message) {
        return noteOpNoThrow(strOpToOp(op), uid, packageName);
    }

    /** The op's mode for the uid, as the file's head says. */
    private int mode(int op, int uid) {
        int switchOp = AppOpsTable.SWITCH_OPS[op];
        String permission = AppOpsTable.PERMISSIONS[switchOp];
        if (permission != null && sRuntimePermissionOp[switchOp]) {
            return mContext.checkPermission(permission, Process.myPid(), uid) == PackageManager.PERMISSION_GRANTED
                    ? MODE_ALLOWED : MODE_IGNORED;
        }
        return AppOpsTable.DEFAULT_MODES[switchOp];
    }

    /** As AOSP's checkOp and noteOp: SecurityException with buildSecurityExceptionMsg's message for MODE_ERRORED. */
    private static int unlessErrored(int op, int uid, String packageName, int mode) {
        if (mode == MODE_ERRORED) {
            throw new SecurityException(packageName + " from uid " + uid + " not allowed to perform "
                    + AppOpsTable.SIMPLE_NAMES[op]);
        }
        return mode;
    }

    /** As AOSP's: the longs of a bitmask over all app ops (_NUM_OP, the length of the table appopsgen writes). */
    private static final int BITMASK_LEN = ((AppOpsTable.NAMES.length - 1) / Long.SIZE) + 1;

    /**
     * @hide Parcel.writeException's and writeNoException's header for the app ops noted in a binder transaction,
     * which AOSP writes only when there are some (ADR-0043 d2): without transactions from other processes there are
     * none to report.
     */
    public static void prefixParcelWithAppOpsIfNeeded(Parcel p) {
    }

    /**
     * @hide Parcel.readExceptionCode's reader of that header: AOSP's reads, so that the parcel's position goes past
     * it, and the ops go to no callback, as no other process noted them.
     */
    public static void readAndLogNotedAppops(Parcel p) {
        int numAttributionsWithNotedAppOps = p.readInt();
        for (int i = 0; i < numAttributionsWithNotedAppOps; i++) {
            p.readString();
            for (int j = 0; j < BITMASK_LEN; j++) {
                p.readLong();
            }
        }
    }
}
