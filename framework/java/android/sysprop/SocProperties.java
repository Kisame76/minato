/*
 * Our stand-in for the sysprop library's generated class (system/libsysprop, which we do not fetch), with the
 * accessors b3.1's Build calls in its static initializer, so they must run (ADR-0039 b3.1). They read the property
 * area as the generated code does: an empty value is none, a list is split at commas. The property names are
 * libsysprop's [inferred: from memory of android/sysprop/*.sysprop; test 3 compares Build's values with the AVD's].
 */
package android.sysprop;

import android.os.SystemProperties;

import java.util.Optional;

public final class SocProperties {
    private SocProperties() {
    }

    public static Optional<String> soc_manufacturer() {
        return Optional.ofNullable(SyspropValues.string(SystemProperties.get("ro.soc.manufacturer")));
    }

    public static Optional<String> soc_model() {
        return Optional.ofNullable(SyspropValues.string(SystemProperties.get("ro.soc.model")));
    }
}
