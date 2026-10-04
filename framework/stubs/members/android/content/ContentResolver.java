// Hidden members of the public API's compile-only stub (ADR-0039 step c1), which tools/managergen joins
// with it: FileUtils' MIME type helpers name the default type. javac inlines the constant, so it carries AOSP's value.
class ContentResolver {
    /** @hide */
    public static final String MIME_TYPE_DEFAULT = "application/octet-stream";
}
