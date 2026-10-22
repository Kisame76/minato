/*
 * The build flags of AOSP's TelephonyUtils (frameworks/opt/telephony), which the cell family and the telephony data
 * types read (Rlog.pii, CellIdentityCdma.toString). The rest of that class belongs to the telephony service (carrier
 * privileges, handler posting, permission checks over the binder) and is not taken; the flags are AOSP's definitions.
 */
package com.android.internal.telephony.util;

import android.os.Build;
import android.os.SystemProperties;

public final class TelephonyUtils {
    private TelephonyUtils() {
    }

    /** Whether this is a user build, which keeps personal data out of logs. */
    public static final boolean IS_USER = "user".equals(Build.TYPE);

    /** Whether this is a debuggable build. */
    public static final boolean IS_DEBUGGABLE = SystemProperties.getInt("ro.debuggable", 0) == 1;
}
