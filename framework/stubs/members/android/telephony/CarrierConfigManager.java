// Hidden members of the public API's compile-only stub (F4d), which tools/managergen joins with it: the keys by which
// the cell signal strengths read a carrier's thresholds (CellSignalStrengthGsm, CellSignalStrengthLte,
// CellSignalStrengthNr and CellSignalStrengthWcdma updateLevel). javac inlines the names, so they carry AOSP's values.
class CarrierConfigManager {
    /** @hide */
    public static final String KEY_GSM_RSSI_THRESHOLDS_INT_ARRAY = "gsm_rssi_thresholds_int_array";

    /** @hide */
    public static final String KEY_LTE_RSRP_THRESHOLDS_INT_ARRAY = "lte_rsrp_thresholds_int_array";

    /** @hide */
    public static final String KEY_PARAMETERS_USED_FOR_LTE_SIGNAL_BAR_INT = "parameters_used_for_lte_signal_bar_int";

    /** @hide */
    public static final String KEY_PARAMETERS_USE_FOR_5G_NR_SIGNAL_BAR_INT = "parameters_use_for_5g_nr_signal_bar_int";

    /** @hide */
    public static final String KEY_USE_ONLY_RSRP_FOR_LTE_SIGNAL_BAR_BOOL = "use_only_rsrp_for_lte_signal_bar_bool";

    /** @hide */
    public static final String KEY_WCDMA_DEFAULT_SIGNAL_STRENGTH_MEASUREMENT_STRING =
            "wcdma_default_signal_strength_measurement_string";

    /** @hide */
    public static final String KEY_WCDMA_RSCP_THRESHOLDS_INT_ARRAY = "wcdma_rscp_thresholds_int_array";
}
