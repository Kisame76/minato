// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ActivityInfo and CompatibilityInfo call them.
class ApplicationInfo {
    /** @hide */
    public String[] getSplitCodePaths() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isChangeEnabled(long changeId) {
        throw new RuntimeException("stub");
    }
}
