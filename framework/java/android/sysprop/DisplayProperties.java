/*
 * Our stand-in for the sysprop library's generated class (system/libsysprop, which we do not fetch), with the accessors
 * b3.4's TextUtils calls for a locale's layout direction (ADR-0039 b3.4), step c2c2's View.AttachInfo for the layout
 * bounds developer option and the one DisplayManagerGlobal calls as it loads (ADR-0041 a)). It reads the property area
 * as the generated code does: an empty value is none, a boolean is "true" or "1" against "false" or "0". The property
 * names are libsysprop's [verified: srcs/android/sysprop/DisplayProperties.sysprop at android-15.0.0_r1]; Minato's
 * area sets none of them, so they are none, as on a device without the developer options.
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

    public static Optional<String> debug_vri_package() {
        return Optional.ofNullable(SyspropValues.string(SystemProperties.get("persist.debug.vri_package")));
    }
}
