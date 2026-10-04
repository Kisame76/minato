// Hidden members of the public API's compile-only stub (ADR-0043 test 2), which tools/managergen joins with it:
// Drawable and AnimatedVectorDrawable name the resolved layout direction's annotation; Context's autofill IDs start
// after the last one an app may use (step c1). javac inlines the constant, so it carries AOSP's value.
class View {
    /** @hide */
    public static final int LAST_APP_AUTOFILL_ID = Integer.MAX_VALUE / 2;

    /** @hide */
    public @interface ResolvedLayoutDir {
    }
}
