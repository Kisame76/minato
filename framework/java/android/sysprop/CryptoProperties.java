/*
 * Our stand-in for the sysprop library's generated class (system/libsysprop, which we do not fetch), with the accessors
 * RoSystemProperties reads in its static initializer, for SSLCertificateSocketFactory. It reads the property area as
 * the generated code does: an empty or unknown value is none. The property names and the value lists are those of
 * system/libsysprop's srcs/android/sysprop/CryptoProperties.sysprop at android-15.0.0_r1: ro.crypto.state (encrypted,
 * unencrypted, unsupported) and ro.crypto.type (block, file, none).
 */
package android.sysprop;

import android.os.SystemProperties;

import java.util.Optional;

public final class CryptoProperties {
    private CryptoProperties() {
    }

    public enum state_values {
        UNSUPPORTED("unsupported"),
        UNENCRYPTED("unencrypted"),
        ENCRYPTED("encrypted");

        private final String value;

        state_values(String value) {
            this.value = value;
        }

        static Optional<state_values> parse(String value) {
            for (state_values candidate : values()) {
                if (candidate.value.equals(value)) return Optional.of(candidate);
            }
            return Optional.empty();
        }
    }

    public enum type_values {
        NONE("none"),
        BLOCK("block"),
        FILE("file");

        private final String value;

        type_values(String value) {
            this.value = value;
        }

        static Optional<type_values> parse(String value) {
            for (type_values candidate : values()) {
                if (candidate.value.equals(value)) return Optional.of(candidate);
            }
            return Optional.empty();
        }
    }

    public static Optional<state_values> state() {
        return state_values.parse(SystemProperties.get("ro.crypto.state"));
    }

    public static Optional<type_values> type() {
        return type_values.parse(SystemProperties.get("ro.crypto.type"));
    }
}
