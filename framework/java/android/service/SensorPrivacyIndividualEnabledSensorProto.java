/*
 * Our stand-in for the class the proto compiler generates from frameworks/base's core/proto/android/hardware/
 * sensorprivacy.proto at android-15.0.0_r1 (not fetched), with the constants SensorPrivacyManager reads: the values of
 * the enums nested in SensorPrivacyIndividualEnabledSensorProto, as that file numbers them.
 */
package android.service;

public final class SensorPrivacyIndividualEnabledSensorProto {
    private SensorPrivacyIndividualEnabledSensorProto() {
    }

    public static final int UNKNOWN = 0;
    public static final int MICROPHONE = 1;
    public static final int CAMERA = 2;
    public static final int SOFTWARE = 1;
    public static final int HARDWARE = 2;
    public static final int ENABLED = 1;
    public static final int DISABLED = 2;
    public static final int ENABLED_EXCEPT_ALLOWLISTED_APPS = 3;
}
