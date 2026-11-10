package com.android.internal.telephony.flags;

/**
 * Ours in place of aconfig's generated com.android.internal.telephony.flags.Flags, whose declaration is
 * frameworks/opt/telephony's and not fetched: the flags' names that Settings, Telephony and PhoneNumberUtils give
 * @FlaggedApi (javac inlines a name, and core/api/current.txt names it), and the one flag PhoneNumberUtils reads at run
 * time, with the value of Android 15's release configuration (build_release/aconfig/ap3a): ENABLED, READ_ONLY. It was a
 * compile-only stub for the names until PhoneNumberUtils needed the method.
 */
public final class Flags {
    private Flags() {}

    public static final String FLAG_CARRIER_ENABLED_SATELLITE_FLAG =
            "com.android.internal.telephony.flags.carrier_enabled_satellite_flag";
    public static final String FLAG_APN_SETTING_FIELD_SUPPORT_FLAG =
            "com.android.internal.telephony.flags.apn_setting_field_support_flag";
    public static final String FLAG_OEM_ENABLED_SATELLITE_FLAG =
            "com.android.internal.telephony.flags.oem_enabled_satellite_flag";
    public static final String FLAG_ENABLE_WPS_CHECK_API_FLAG =
            "com.android.internal.telephony.flags.enable_wps_check_api_flag";

    /** Whether a local Singapore call is shown without its country code. */
    public static boolean removeCountryCodeFromLocalSingaporeCalls() {
        return true;
    }
}
