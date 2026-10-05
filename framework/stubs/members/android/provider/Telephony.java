// Hidden members of the public API's compile-only stub (ADR-0045, gap 13a), which tools/managergen joins with it:
// Settings.Global's IMS keys are SimInfo's column names; javac inlines them, so they carry AOSP's values.
class Telephony {
    /** @hide */
    public static final class SimInfo {
        /** @hide */
        public static final String COLUMN_ENHANCED_4G_MODE_ENABLED = "volte_vt_enabled";

        /** @hide */
        public static final String COLUMN_VT_IMS_ENABLED = "vt_ims_enabled";

        /** @hide */
        public static final String COLUMN_WFC_IMS_ENABLED = "wfc_ims_enabled";

        /** @hide */
        public static final String COLUMN_WFC_IMS_MODE = "wfc_ims_mode";

        /** @hide */
        public static final String COLUMN_WFC_IMS_ROAMING_MODE = "wfc_ims_roaming_mode";

        /** @hide */
        public static final String COLUMN_WFC_IMS_ROAMING_ENABLED = "wfc_ims_roaming_enabled";
    }
}
