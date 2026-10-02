// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ParcelFileDescriptor calls them (b3.3).
class FileUtils {
    /** @hide */
    public static int translateModeStringToPosix(String mode) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static int translateModePfdToPosix(int mode) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static int translateModePosixToPfd(int mode) {
        throw new RuntimeException("stub");
    }
}
