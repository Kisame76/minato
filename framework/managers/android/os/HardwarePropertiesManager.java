// The written part of HardwarePropertiesManager (services bundle A of the system services gap): CPU usages, fan
// speeds and device temperatures. An app holds neither the DEVICE_POWER permission nor is a device owner or the VR
// listener, so each call is a SecurityException, as the emulator (apk-runner-35-bare) answers
// [verified: services-f-test]; the message is AOSP's [inferred from android-15.0.0_r1,
// HardwarePropertiesManagerService.enforceHardwarePropertiesRetrievalAllowed].
import android.content.Context;

class HardwarePropertiesManager {
    /** @hide As the tag's, which SystemServiceRegistry calls. */
    public HardwarePropertiesManager(Context context) {
    }

    private static SecurityException notAllowed() {
        return new SecurityException("The caller is neither a device owner, nor holding the DEVICE_POWER "
                + "permission, nor the current VrListener.");
    }

    public float[] getDeviceTemperatures(int type, int source) {
        throw notAllowed();
    }

    public CpuUsageInfo[] getCpuUsages() {
        throw notAllowed();
    }

    public float[] getFanSpeeds() {
        throw notAllowed();
    }
}
