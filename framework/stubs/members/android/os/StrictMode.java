// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// Resources calls it.
class StrictMode {
    /** @hide */
    public static void noteResourceMismatch(Object tag) {
        throw new RuntimeException("stub");
    }
}
