// Hidden members of the public API's compile-only stub (ADR-0045's storage and app ops step), which tools/managergen
// joins with it: StorageVolume's intents for the documents UI name the external storage provider's primary root.
// javac inlines the constants, so they carry AOSP's values.

class DocumentsContract {
    /** @hide */
    public static final String EXTRA_SHOW_ADVANCED = "android.provider.extra.SHOW_ADVANCED";

    /** @hide */
    public static final String EXTERNAL_STORAGE_PROVIDER_AUTHORITY = "com.android.externalstorage.documents";

    /** @hide */
    public static final String EXTERNAL_STORAGE_PRIMARY_EMULATED_ROOT_ID = "primary";
}
