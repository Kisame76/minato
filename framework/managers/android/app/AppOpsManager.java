// The hand file of our AppOpsManager (ADR-0039 step c2c1, ADR-0045 decision 3): tools/managergen writes the class
// from the public API, whose methods log once and return their default, and the members here are the hidden ones
// that taken AOSP files call. ContentProvider.attachInfo casts the app ops service to this class, which no Context
// gives yet (ADR-0045's local services): so the class must exist, and no permission has an app op on Minato.
import com.android.minato.services.Unwritten;

class AppOpsManager {
    /** @hide No app op, as for a permission without one. */
    public static final int OP_NONE = -1;

    /** @hide As AOSP's: the ops Environment's legacy storage checks ask by number (step c1). */
    public static final int OP_LEGACY_STORAGE = 87;

    /** @hide */
    public static final int OP_MANAGE_EXTERNAL_STORAGE = 92;

    /** @hide */
    public static final int OP_NO_ISOLATED_STORAGE = 99;

    /** @hide No permission has an app op on Minato, so ContentProvider notes none. */
    public static int permissionToOpCode(String permission) {
        return OP_NONE;
    }

    /** @hide */
    public static String opToPublicName(int op) {
        Unwritten.log("android.app.AppOpsManager.opToPublicName(int)", "null");
        return null;
    }

    /** @hide */
    public int noteOpNoThrow(int op, int uid, String packageName) {
        Unwritten.log("android.app.AppOpsManager.noteOpNoThrow(int,int,String)", "0");
        return 0;
    }

    /** @hide */
    public int checkOpNoThrow(int op, int uid, String packageName) {
        Unwritten.log("android.app.AppOpsManager.checkOpNoThrow(int,int,String)", "0");
        return 0;
    }
}
