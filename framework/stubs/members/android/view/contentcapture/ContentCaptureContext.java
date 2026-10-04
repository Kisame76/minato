// Hidden members of the public API's compile-only stub (ADR-0039 step c2c1), which tools/managergen joins with it:
// ContentCaptureManager's flags; javac inlines them with AOSP's values.
class ContentCaptureContext {
    /** @hide */
    public static final int FLAG_DISABLED_BY_APP = 0x1;

    /** @hide */
    public static final int FLAG_DISABLED_BY_FLAG_SECURE = 0x2;

    /** @hide */
    public static final int FLAG_DISABLED_FLUSH_FOR_VIEW_TREE_APPEARING = 1 << 3;
}
