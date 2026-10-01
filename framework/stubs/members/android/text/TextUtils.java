// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// Log, ResourcesImpl, Parcelling and others call them.
class TextUtils {
    /** @hide */
    public static String safeIntern(String s) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static String emptyIfNull(String str) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static String formatSimple(String format, Object... args) {
        throw new RuntimeException("stub");
    }
}
