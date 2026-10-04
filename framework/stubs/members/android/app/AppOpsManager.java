// Hidden members of the public API's compile-only stub (ADR-0039 step c1), which tools/managergen joins with it:
// Environment's isExternalStorageLegacy and isExternalStorageManager ask these app ops by number. javac inlines the
// constants, so they carry AOSP's values: each op's place in AppOpsManager's sAppOpInfos, which AppProtoEnums numbers
// alike.
class AppOpsManager {
    /** @hide */
    public static final int OP_LEGACY_STORAGE = 87;

    /** @hide */
    public static final int OP_MANAGE_EXTERNAL_STORAGE = 92;

    /** @hide */
    public static final int OP_NO_ISOLATED_STORAGE = 99;

    /** @hide */
    public int noteOpNoThrow(int op, int uid, String packageName) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int checkOpNoThrow(int op, int uid, String packageName) {
        throw new RuntimeException("stub");
    }
}
