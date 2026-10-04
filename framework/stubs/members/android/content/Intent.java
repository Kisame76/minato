// Hidden members of the public API's compile-only stub (ADR-0039 step c1), which tools/managergen joins with it:
// Context's URI permission methods and its PendingIntent flags name these source-only annotations.
class Intent {
    /** @hide */
    public @interface GrantUriMode {
    }

    /** @hide */
    public @interface AccessUriMode {
    }

    /** @hide */
    public @interface MutableFlags {
    }
}
