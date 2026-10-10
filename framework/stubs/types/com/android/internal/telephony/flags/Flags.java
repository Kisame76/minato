package com.android.internal.telephony.flags;

/**
 * Compile-only stub (ADR-0045, gap 13a), never in framework.jar: the telephony flags' names that Settings and
 * Telephony give @FlaggedApi (Telephony's APN setting field, retest 8's batch B1). Their declaration is frameworks/opt/telephony's, which is not fetched; javac inlines the name, which
 * core/api/current.txt names.
 */
public final class Flags {
    public static final String FLAG_CARRIER_ENABLED_SATELLITE_FLAG =
            "com.android.internal.telephony.flags.carrier_enabled_satellite_flag";
    public static final String FLAG_APN_SETTING_FIELD_SUPPORT_FLAG =
            "com.android.internal.telephony.flags.apn_setting_field_support_flag";
}
