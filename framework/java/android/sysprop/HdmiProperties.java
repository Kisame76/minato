/*
 * Our stand-in for the sysprop library's generated class (system/libsysprop, which we do not fetch), with the one
 * accessor RoSystemProperties reads in its static initializer, for SSLCertificateSocketFactory. It reads the property
 * area as the generated code does: an empty value is none. The property name is that of system/libsysprop's
 * srcs/android/sysprop/HdmiProperties.sysprop at android-15.0.0_r1.
 */
package android.sysprop;

import android.os.SystemProperties;

import java.util.Optional;

public final class HdmiProperties {
    private HdmiProperties() {
    }

    public static Optional<Boolean> forward_volume_keys_when_system_audio_mode_off() {
        return Optional.ofNullable(SyspropValues.bool(
                SystemProperties.get("ro.hdmi.cec_audio_device_forward_volume_keys_system_audio_mode_off")));
    }
}
