/*
 * Our stand-in for the sysprop library's generated class (system/libsysprop, which we do not fetch), with the
 * accessors b3.1's Build calls in its static initializer, so they must run (ADR-0039 b3.1). They read the property
 * area as the generated code does: an empty value is none, a list is split at commas. The property names are
 * libsysprop's [inferred: from memory of android/sysprop/*.sysprop; test 3 compares Build's values with the AVD's].
 */
package android.sysprop;

import java.util.ArrayList;
import java.util.List;

/**
 * The generated sysprop classes' parsing: an empty value is none, a boolean is "true" or "1" against "false" or "0",
 * and a list is split at commas.
 */
final class SyspropValues {
    private SyspropValues() {
    }

    static String string(String value) {
        return value.isEmpty() ? null : value;
    }

    static Boolean bool(String value) {
        if (value.equals("true") || value.equals("1")) {
            return Boolean.TRUE;
        }
        if (value.equals("false") || value.equals("0")) {
            return Boolean.FALSE;
        }
        return null;
    }

    static Integer integer(String value) {
        try {
            return Integer.valueOf(value);
        } catch (NumberFormatException none) {
            return null;
        }
    }

    static List<String> list(String value) {
        List<String> values = new ArrayList<>();
        if (value.isEmpty()) {
            return values;
        }
        for (String item : value.split(",", -1)) {
            values.add(string(item));
        }
        return values;
    }
}
