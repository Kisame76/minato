// Hidden members of the public API's compile-only stub (ADR-0039 b3.1), which tools/managergen joins with it:
// ActivityInfo's dump names them.
class PackageItemInfo {
    /** @hide */
    public static final int DUMP_FLAG_DETAILS = 1 << 0;

    /** @hide */
    public static final int DUMP_FLAG_APPLICATION = 1 << 1;

    /** @hide */
    public static final int DUMP_FLAG_ALL = DUMP_FLAG_DETAILS | DUMP_FLAG_APPLICATION;
}
