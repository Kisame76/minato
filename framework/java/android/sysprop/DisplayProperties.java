/*
 * Our stand-in for the sysprop library's generated class (system/libsysprop, which we do not fetch), with the accessors
 * b3.4's TextUtils calls for a locale's layout direction (ADR-0039 b3.4) and step c2c2's View.AttachInfo for the layout
 * bounds developer option. It reads the property area as the generated code does: an empty value is none, a boolean is
 * "true" or "1" against "false" or "0". The property name is libsysprop's [inferred: from memory of
 * android/sysprop/DisplayProperties.sysprop; Minato's area does not set it, so it is none, as on a device without the
 * developer option].
 */
package android.sysprop;

import android.os.SystemProperties;

import java.util.Optional;

public final class DisplayProperties {
    private DisplayProperties() {
    }

    public static Optional<Boolean> debug_force_rtl() {
        return Optional.ofNullable(SyspropValues.bool(SystemProperties.get("debug.force_rtl")));
    }

    public static Optional<Boolean> debug_layout() {
        return Optional.ofNullable(SyspropValues.bool(SystemProperties.get("debug.layout")));
    }
}
