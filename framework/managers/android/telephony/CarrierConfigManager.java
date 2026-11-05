// The written part of CarrierConfigManager (services batch B): the carrier's settings, which an app without
// READ_PHONE_STATE or carrier privileges cannot read: Android 15 answers a SecurityException
// [verified: services-b-test].
import android.content.Context;

class CarrierConfigManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public CarrierConfigManager(Context context) {
        mContext = context;
    }

    public android.os.PersistableBundle getConfig() {
        throw new SecurityException("getConfig: Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.READ_PHONE_STATE.");
    }

    public android.os.PersistableBundle getConfig(String... keys) {
        throw new SecurityException("getConfig: Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.READ_PHONE_STATE.");
    }

    public android.os.PersistableBundle getConfigForSubId(int subId) {
        throw new SecurityException("getConfigForSubId: Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.READ_PHONE_STATE.");
    }

    // The keys by which the cell signal strengths read a carrier's thresholds (CellSignalStrengthGsm, -Lte, -Nr and
    // -Wcdma updateLevel), which the public API lacks; javac inlines the names, so they carry AOSP's values.
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
