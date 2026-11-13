/*
 * Our stand-in for the class the proto compiler generates from frameworks/base's core/proto/android/hardware/
 * sensorprivacy.proto at android-15.0.0_r1 (not fetched), with the constants SensorPrivacyManager reads: the values of
 * the enums nested in SensorPrivacySensorProto, as that file numbers them.
 */
package android.service;

public final class SensorPrivacySensorProto {
    private SensorPrivacySensorProto() {
    }

    public static final int UNKNOWN = 0;
    public static final int MICROPHONE = 1;
    public static final int CAMERA = 2;
}
