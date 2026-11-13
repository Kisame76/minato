/*
 * Our stand-in for the class the proto compiler generates from frameworks/base's core/proto/android/hardware/
 * sensorprivacy.proto at android-15.0.0_r1 (not fetched), with the constants SensorPrivacyManager reads: the values of
 * the enums nested in SensorPrivacyToggleSourceProto, as that file numbers them.
 */
package android.service;

public final class SensorPrivacyToggleSourceProto {
    private SensorPrivacyToggleSourceProto() {
    }

    public static final int UNKNOWN = 0;
    public static final int QS_TILE = 1;
    public static final int SETTINGS = 2;
    public static final int DIALOG = 3;
    public static final int SHELL = 4;
    public static final int OTHER = 5;
    public static final int SAFETY_CENTER = 6;
}
