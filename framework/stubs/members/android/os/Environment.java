// Hidden members of the public API's compile-only stub (ADR-0039 b3.5a), which tools/managergen joins with it:
// ApplicationInfo's data folders for another user or volume, which no app process of Minato asks for.
class Environment {
    /** @hide */
    public static java.io.File getDataSystemDirectory() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static java.io.File getDataUserCePackageDirectory(String volumeUuid, int userId, String packageName) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static java.io.File getDataUserDePackageDirectory(String volumeUuid, int userId, String packageName) {
        throw new RuntimeException("stub");
    }
}
