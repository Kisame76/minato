// Hidden members of the public API's compile-only stub (ADR-0039 b3.5a), which tools/managergen joins with it:
// ApplicationInfo converts its storage UUID with it.
// (ADR-0039 step c2c1) Intent.prepareToLeaveProcess translates file paths for the system.
import java.io.File;

class StorageManager {
    /** @hide */
    public static String convert(java.util.UUID storageUuid) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public File translateAppToSystem(File file, int pid, int uid) {
        throw new RuntimeException("stub");
    }
}
