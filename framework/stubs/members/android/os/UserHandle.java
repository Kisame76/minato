// Hidden members of the public API's compile-only stub (ADR-0039 b3.4), which tools/managergen joins with it:
// PackageItemInfo reads USER_NULL; PackageManager and ApplicationInfo the ids (b3.5a). javac inlines the constant, so
// it carries AOSP's value.
class UserHandle {
    /** @hide */
    public static final int USER_NULL = -10000;

    /** @hide */
    public static int myUserId() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static int getAppId(int uid) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getIdentifier() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static int getUid(int userId, int appId) {
        throw new RuntimeException("stub");
    }
}
