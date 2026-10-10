/*
 * The default SMS app (retest 8's resolve list; Telephony.Sms.getDefaultSmsPackage asks it). Minato has no SMS role
 * holder and no telephony provider, so there is no default SMS application: as AOSP's SmsApplication answers when the
 * role has no holder, getDefaultSmsApplication gives null. AOSP's class (frameworks/opt/telephony) is the role
 * manager's client and is not taken.
 */
package com.android.internal.telephony;

import android.content.ComponentName;
import android.content.Context;

public final class SmsApplication {
    private SmsApplication() {
    }

    /** The component of the default SMS app, or null while the SMS role has no holder, as on Minato. */
    public static ComponentName getDefaultSmsApplication(Context context, boolean updateIfNeeded) {
        return null;
    }
}
