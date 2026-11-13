// The written part of AudioDeviceInfo (the Unity test APK: FMOD lists the outputs before it builds its
// AudioTrack), which tools/managergen joins with the API's methods. Android's wraps an audio port of the audio server;
// ours is a device AudioManager.getDevices makes: its type and ID, a sink or a source. The product name of a built-in
// device is Build.MODEL's property, as AOSP's getProductName gives Build.MODEL for a port without a name [from
// memory]; Build itself is not named, as its annotations stop tools/managers-check.sh's compile. Empty lists of
// rates, masks, counts and encodings mean that the device takes any, as the API says.
import android.os.SystemProperties;
import android.util.SparseIntArray;

import java.util.ArrayList;
import java.util.List;

class AudioDeviceInfo {
    /** @hide AudioDeviceAttributes' annotation of a device type. */
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.SOURCE)
    public @interface AudioDeviceType {}

    /**
     * @hide The system-API effects' device (AudioEffect for a device) is not offered: the type goes on as it is,
     * where AOSP maps it to the audio HAL's device bit.
     */
    public static int convertDeviceTypeToInternalDevice(int deviceType) {
        return deviceType;
    }

    /** @hide As convertDeviceTypeToInternalDevice, for an input and its address. */
    public static int convertDeviceTypeToInternalInputDevice(int deviceType, String address) {
        return deviceType;
    }

    /** @hide AudioDeviceAttributes' checks of a device type; every type the API names is taken as valid. */
    public static void enforceValidAudioDeviceTypeOut(int deviceType) {
    }

    /** @hide */
    public static void enforceValidAudioDeviceTypeIn(int deviceType) {
    }

    /** @hide The type as the audio HAL names it: ours is the API's. */
    public int getInternalType() {
        return mType;
    }

    private int mId;
    private int mType;
    private boolean mSink;

    /** @hide AudioManager's, for a device of the session's output or input. */
    AudioDeviceInfo(int id, int type, boolean sink) {
        mId = id;
        mType = type;
        mSink = sink;
    }

    public int getId() {
        return mId;
    }

    public CharSequence getProductName() {
        return SystemProperties.get("ro.product.model", "unknown");
    }

    public String getAddress() {
        return "";
    }

    public boolean isSource() {
        return !mSink;
    }

    public boolean isSink() {
        return mSink;
    }

    public int[] getSampleRates() {
        return new int[0];
    }

    public int[] getChannelMasks() {
        return new int[0];
    }

    public int[] getChannelIndexMasks() {
        return new int[0];
    }

    public int[] getChannelCounts() {
        return new int[0];
    }

    public int[] getEncodings() {
        return new int[0];
    }

    public int[] getEncapsulationModes() {
        return new int[0];
    }

    public int[] getEncapsulationMetadataTypes() {
        return new int[0];
    }

    public List<AudioProfile> getAudioProfiles() {
        return new ArrayList<>();
    }

    public List<AudioDescriptor> getAudioDescriptors() {
        return new ArrayList<>();
    }

    public int getType() {
        return mType;
    }

    /** @hide The API type of a native device type, TYPE_UNKNOWN for one with none; AOSP's table, verbatim. */
    public static int convertInternalDeviceToDeviceType(int intDevice) {
        return INT_TO_EXT_DEVICE_MAPPING.get(intDevice, TYPE_UNKNOWN);
    }

    /** @hide The echo reference input [verified: AudioDeviceInfo.java at android-15.0.0_r1]. */
    public static final int TYPE_ECHO_REFERENCE = 28;

    private static final SparseIntArray INT_TO_EXT_DEVICE_MAPPING;

    static {
            INT_TO_EXT_DEVICE_MAPPING = new SparseIntArray();
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_EARPIECE, TYPE_BUILTIN_EARPIECE);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_SPEAKER, TYPE_BUILTIN_SPEAKER);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_WIRED_HEADSET, TYPE_WIRED_HEADSET);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_WIRED_HEADPHONE, TYPE_WIRED_HEADPHONES);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLUETOOTH_SCO, TYPE_BLUETOOTH_SCO);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLUETOOTH_SCO_HEADSET, TYPE_BLUETOOTH_SCO);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLUETOOTH_SCO_CARKIT, TYPE_BLUETOOTH_SCO);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLUETOOTH_A2DP, TYPE_BLUETOOTH_A2DP);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES, TYPE_BLUETOOTH_A2DP);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER, TYPE_BLUETOOTH_A2DP);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_HDMI, TYPE_HDMI);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_ANLG_DOCK_HEADSET, TYPE_DOCK_ANALOG);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_DGTL_DOCK_HEADSET, TYPE_DOCK);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_USB_ACCESSORY, TYPE_USB_ACCESSORY);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_USB_DEVICE, TYPE_USB_DEVICE);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_USB_HEADSET, TYPE_USB_HEADSET);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_TELEPHONY_TX, TYPE_TELEPHONY);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_LINE, TYPE_LINE_ANALOG);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_HDMI_ARC, TYPE_HDMI_ARC);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_HDMI_EARC, TYPE_HDMI_EARC);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_SPDIF, TYPE_LINE_DIGITAL);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_FM, TYPE_FM);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_AUX_LINE, TYPE_AUX_LINE);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_IP, TYPE_IP);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BUS, TYPE_BUS);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_HEARING_AID, TYPE_HEARING_AID);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_SPEAKER_SAFE,
                    TYPE_BUILTIN_SPEAKER_SAFE);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_REMOTE_SUBMIX, TYPE_REMOTE_SUBMIX);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLE_HEADSET, TYPE_BLE_HEADSET);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLE_SPEAKER, TYPE_BLE_SPEAKER);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_OUT_BLE_BROADCAST, TYPE_BLE_BROADCAST);

            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_BUILTIN_MIC, TYPE_BUILTIN_MIC);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_BLUETOOTH_SCO_HEADSET, TYPE_BLUETOOTH_SCO);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_WIRED_HEADSET, TYPE_WIRED_HEADSET);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_HDMI, TYPE_HDMI);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_TELEPHONY_RX, TYPE_TELEPHONY);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_BACK_MIC, TYPE_BUILTIN_MIC);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_ANLG_DOCK_HEADSET, TYPE_DOCK_ANALOG);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_DGTL_DOCK_HEADSET, TYPE_DOCK);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_USB_ACCESSORY, TYPE_USB_ACCESSORY);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_USB_DEVICE, TYPE_USB_DEVICE);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_USB_HEADSET, TYPE_USB_HEADSET);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_FM_TUNER, TYPE_FM_TUNER);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_TV_TUNER, TYPE_TV_TUNER);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_LINE, TYPE_LINE_ANALOG);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_SPDIF, TYPE_LINE_DIGITAL);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_BLUETOOTH_A2DP, TYPE_BLUETOOTH_A2DP);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_IP, TYPE_IP);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_BUS, TYPE_BUS);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_REMOTE_SUBMIX, TYPE_REMOTE_SUBMIX);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_BLE_HEADSET, TYPE_BLE_HEADSET);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_HDMI_ARC, TYPE_HDMI_ARC);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_HDMI_EARC, TYPE_HDMI_EARC);
            INT_TO_EXT_DEVICE_MAPPING.put(AudioSystem.DEVICE_IN_ECHO_REFERENCE, TYPE_ECHO_REFERENCE);
    }
}
