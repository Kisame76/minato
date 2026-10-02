// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ActivityInfo, CompatibilityInfo and ComponentInfo (b3.4) call them, and ResourcesManager and LocaleConfig (b3.4).
class ApplicationInfo {
    /** @hide */
    public String[] resourceDirs;

    /** @hide */
    public String[] overlayPaths;

    /** @hide */
    public String getBaseCodePath() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public int getLocaleConfigRes() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public String[] getSplitCodePaths() {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public boolean isChangeEnabled(long changeId) {
        throw new RuntimeException("stub");
    }

    /** @hide */
    public void dump(android.util.Printer pw, String prefix, int dumpFlags) {
        throw new RuntimeException("stub");
    }
}
