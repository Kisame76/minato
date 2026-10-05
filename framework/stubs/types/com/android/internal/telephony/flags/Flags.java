package com.android.internal.telephony.flags;

/**
 * Compile-only stub (ADR-0045, gap 13a), never in framework.jar: the telephony flag's name that Settings gives
 * @FlaggedApi. Its declaration is frameworks/opt/telephony's, which is not fetched; javac inlines the name, which
 * core/api/current.txt names.
 */
public final class Flags {
    public static final String FLAG_CARRIER_ENABLED_SATELLITE_FLAG =
            "com.android.internal.telephony.flags.carrier_enabled_satellite_flag";
}
