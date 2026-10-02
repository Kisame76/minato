// Hidden members of the public API's compile-only stub (ADR-0039 b3.4), which tools/managergen joins with it:
// PackageItemInfo reads USER_NULL. javac inlines the constant, so it carries AOSP's value.
class UserHandle {
    /** @hide */
    public static final int USER_NULL = -10000;
}
