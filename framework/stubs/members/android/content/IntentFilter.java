// Hidden members of the public API's compile-only stub (ADR-0039 step c2c1), which tools/managergen joins with it:
// Intent.isWebIntent compares the scheme; javac inlines them.
class IntentFilter {
    /** @hide */
    public static final String SCHEME_HTTP = "http";

    /** @hide */
    public static final String SCHEME_HTTPS = "https";
}
