/*
 * Our aconfig flags of Telecom (package com.android.server.telecom.flags), whose declarations are not among the
 * fetched sources: the names that CallLog's and Context's @FlaggedApi give and the one flag CallLog's AddCallParams
 * reads. business_call_composer is ENABLED, READ_ONLY in build_release's ap3a set [verified: build_release/aconfig/
 * ap3a/com.android.server.telecom.flags/business_call_composer_flag_values.textproto].
 */
package com.android.server.telecom.flags;

/** @hide */
public final class Flags {
    private Flags() {
    }

    public static final String FLAG_BUSINESS_CALL_COMPOSER = "com.android.server.telecom.flags.business_call_composer";

    /** Named by Context's @FlaggedApi; its aconfig declaration is not among the fetched sources. */
    public static final String FLAG_TELECOM_MAINLINE_BLOCKED_NUMBERS_MANAGER =
            "com.android.server.telecom.flags.telecom_mainline_blocked_numbers_manager";

    /** Named by CallAttributes' @FlaggedApi (batch H4); the name follows the flags' pattern [inferred]. */
    public static final String FLAG_TRANSACTIONAL_VIDEO_STATE =
            "com.android.server.telecom.flags.transactional_video_state";

    public static boolean businessCallComposer() {
        return true;
    }
}
