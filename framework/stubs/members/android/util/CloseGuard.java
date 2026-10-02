// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ParcelFileDescriptor calls them (b3.3).
class CloseGuard {
    /** @hide */
    public static CloseGuard get() {
        throw new RuntimeException("stub");
    }
}
