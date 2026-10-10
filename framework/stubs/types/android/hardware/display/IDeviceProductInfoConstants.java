package android.hardware.display;

/**
 * Compile-only stub (App, the Unity test APK retest 11), never in framework.jar: the AIDL constants DeviceProductInfo's
 * CONNECTION_TO_SINK_* copy, which javac inlines; the values are the public API's.
 */
public interface IDeviceProductInfoConstants {
    int CONNECTION_TO_SINK_UNKNOWN = 0;
    int CONNECTION_TO_SINK_BUILT_IN = 1;
    int CONNECTION_TO_SINK_DIRECT = 2;
    int CONNECTION_TO_SINK_TRANSITIVE = 3;
}
