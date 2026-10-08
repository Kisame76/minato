// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// FragmentManager logs its callers. (The framework breadth's batch 2) ViewDebug counts the views and roots.
class Debug {
    /** @hide */
    public static String getCallers(final int depth) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public static long countInstancesOfClass(Class cls) {
        throw new RuntimeException("stub");
    }
}
