// The written part of AudioSystem (the F4g slice of ADR-0064): the stream types and limits that were a compile-only
// stub (Android 15's natives return the limits, android_media_AudioSystem.cpp [read]), and the plain
// Java of AOSP's AudioSystem that names devices: the error codes, the native device types and their names, as
// android-15.0.0_r1 has them, verbatim from "Error codes" to the device names. The natives of the audio server are
// not here; the audio manager step owns them.
package android.media;

import android.annotation.IntDef;
import android.annotation.NonNull;
import android.compat.annotation.UnsupportedAppUsage;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.TreeSet;

/** @hide */
public class AudioSystem {
    public static final int STREAM_VOICE_CALL = 0;
    public static final int STREAM_SYSTEM = 1;
    public static final int STREAM_RING = 2;
    public static final int STREAM_MUSIC = 3;
    public static final int STREAM_ALARM = 4;
    public static final int STREAM_NOTIFICATION = 5;
    public static final int STREAM_BLUETOOTH_SCO = 6;
    public static final int STREAM_SYSTEM_ENFORCED = 7;
    public static final int STREAM_DTMF = 8;
    public static final int STREAM_TTS = 9;
    public static final int STREAM_ACCESSIBILITY = 10;
    public static final int STREAM_ASSISTANT = 11;
    public static final int STREAM_DEFAULT = -1;
    public static final int OUT_CHANNEL_COUNT_MAX = 28;
    public static final int FCC_24 = 24;
    public static final int SAMPLE_RATE_HZ_MAX = 192000;
    public static final int SAMPLE_RATE_HZ_MIN = 4000;

    /*
     * Error codes used by public APIs (AudioTrack, AudioRecord, AudioManager ...)
     * Must be kept in sync with frameworks/base/core/jni/android_media_AudioErrors.h
     */
    /** @hide */
    public static final int AUDIO_SESSION_ALLOCATE = 0;  // MediaPlayer's (ADR-0060 e2e1)

    /** @hide */
    public static final int SUCCESS            = 0;
    /** @hide */
    public static final int ERROR              = -1;
    /** @hide */
    public static final int BAD_VALUE          = -2;
    /** @hide */
    public static final int INVALID_OPERATION  = -3;
    /** @hide */
    public static final int PERMISSION_DENIED  = -4;
    /** @hide */
    public static final int NO_INIT            = -5;
    /** @hide */
    public static final int DEAD_OBJECT        = -6;
    /** @hide */
    public static final int WOULD_BLOCK        = -7;

    /** @hide */
    @IntDef({
            SUCCESS,
            ERROR,
            BAD_VALUE,
            INVALID_OPERATION,
            PERMISSION_DENIED,
            NO_INIT,
            DEAD_OBJECT,
            WOULD_BLOCK
    })
    @Retention(RetentionPolicy.SOURCE)
    public @interface AudioSystemError {}

    /**
     * @hide
     * Convert an int error value to its String value for readability.
     * Accepted error values are the java AudioSystem errors, matching android_media_AudioErrors.h,
     * which map onto the native status_t type.
     * @param error one of the java AudioSystem errors
     * @return a human-readable string
     */
    public static String audioSystemErrorToString(@AudioSystemError int error) {
        switch(error) {
            case SUCCESS:
                return "SUCCESS";
            case ERROR:
                return "ERROR";
            case BAD_VALUE:
                return "BAD_VALUE";
            case INVALID_OPERATION:
                return "INVALID_OPERATION";
            case PERMISSION_DENIED:
                return "PERMISSION_DENIED";
            case NO_INIT:
                return "NO_INIT";
            case DEAD_OBJECT:
                return "DEAD_OBJECT";
            case WOULD_BLOCK:
                return "WOULD_BLOCK";
            default:
                return ("unknown error:" + error);
        }
    }

    /*
     * AudioPolicyService methods
     */

    //
    // audio device definitions: must be kept in sync with values
    // in system/media/audio/include/system/audio-hal-enums.h
    //
    /** @hide */
    public static final int DEVICE_NONE = 0x0;
    // reserved bits
    /** @hide */
    public static final int DEVICE_BIT_IN = 0x80000000;
    /** @hide */
    public static final int DEVICE_BIT_DEFAULT = 0x40000000;
    // output devices, be sure to update AudioManager.java also
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_EARPIECE = 0x1;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_SPEAKER = 0x2;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_WIRED_HEADSET = 0x4;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_WIRED_HEADPHONE = 0x8;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_BLUETOOTH_SCO = 0x10;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_BLUETOOTH_SCO_HEADSET = 0x20;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_BLUETOOTH_SCO_CARKIT = 0x40;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_BLUETOOTH_A2DP = 0x80;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES = 0x100;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER = 0x200;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_AUX_DIGITAL = 0x400;
    /** @hide */
    public static final int DEVICE_OUT_HDMI = DEVICE_OUT_AUX_DIGITAL;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_ANLG_DOCK_HEADSET = 0x800;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_DGTL_DOCK_HEADSET = 0x1000;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_USB_ACCESSORY = 0x2000;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_USB_DEVICE = 0x4000;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_REMOTE_SUBMIX = 0x8000;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_TELEPHONY_TX = 0x10000;
    /** @hide */
    public static final int DEVICE_OUT_LINE = 0x20000;
    /** @hide */
    public static final int DEVICE_OUT_HDMI_ARC = 0x40000;
    /** @hide */
    public static final int DEVICE_OUT_HDMI_EARC = 0x40001;
    /** @hide */
    public static final int DEVICE_OUT_SPDIF = 0x80000;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_FM = 0x100000;
    /** @hide */
    public static final int DEVICE_OUT_AUX_LINE = 0x200000;
    /** @hide */
    public static final int DEVICE_OUT_SPEAKER_SAFE = 0x400000;
    /** @hide */
    public static final int DEVICE_OUT_IP = 0x800000;
    /** @hide */
    public static final int DEVICE_OUT_BUS = 0x1000000;
    /** @hide */
    public static final int DEVICE_OUT_PROXY = 0x2000000;
    /** @hide */
    public static final int DEVICE_OUT_USB_HEADSET = 0x4000000;
    /** @hide */
    public static final int DEVICE_OUT_HEARING_AID = 0x8000000;
    /** @hide */
    public static final int DEVICE_OUT_ECHO_CANCELLER = 0x10000000;
    /** @hide */
    public static final int DEVICE_OUT_BLE_HEADSET = 0x20000000;
    /** @hide */
    public static final int DEVICE_OUT_BLE_SPEAKER = 0x20000001;
    /** @hide */
    public static final int DEVICE_OUT_BLE_BROADCAST = 0x20000002;

    /** @hide */
    public static final int DEVICE_OUT_DEFAULT = DEVICE_BIT_DEFAULT;

    // Deprecated in R because multiple device types are no longer accessed as a bit mask.
    // Removing this will get lint warning about changing hidden apis.
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_OUT_ALL_USB = (DEVICE_OUT_USB_ACCESSORY |
                                                  DEVICE_OUT_USB_DEVICE |
                                                  DEVICE_OUT_USB_HEADSET);

    /** @hide */
    public static final Set<Integer> DEVICE_OUT_ALL_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_OUT_ALL_A2DP_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_OUT_ALL_SCO_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_OUT_ALL_USB_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_OUT_ALL_HDMI_SYSTEM_AUDIO_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_ALL_HDMI_SYSTEM_AUDIO_AND_SPEAKER_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_OUT_ALL_BLE_SET;
    static {
        DEVICE_OUT_ALL_SET = new HashSet<>();
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_EARPIECE);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_SPEAKER);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_WIRED_HEADSET);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_WIRED_HEADPHONE);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLUETOOTH_SCO);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLUETOOTH_SCO_HEADSET);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLUETOOTH_SCO_CARKIT);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLUETOOTH_A2DP);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_HDMI);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_ANLG_DOCK_HEADSET);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_DGTL_DOCK_HEADSET);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_USB_ACCESSORY);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_USB_DEVICE);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_REMOTE_SUBMIX);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_TELEPHONY_TX);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_LINE);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_HDMI_ARC);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_HDMI_EARC);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_SPDIF);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_FM);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_AUX_LINE);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_SPEAKER_SAFE);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_IP);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BUS);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_PROXY);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_USB_HEADSET);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_HEARING_AID);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_ECHO_CANCELLER);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLE_HEADSET);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLE_SPEAKER);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_BLE_BROADCAST);
        DEVICE_OUT_ALL_SET.add(DEVICE_OUT_DEFAULT);

        DEVICE_OUT_ALL_A2DP_SET = new HashSet<>();
        DEVICE_OUT_ALL_A2DP_SET.add(DEVICE_OUT_BLUETOOTH_A2DP);
        DEVICE_OUT_ALL_A2DP_SET.add(DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES);
        DEVICE_OUT_ALL_A2DP_SET.add(DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER);

        DEVICE_OUT_ALL_SCO_SET = new HashSet<>();
        DEVICE_OUT_ALL_SCO_SET.add(DEVICE_OUT_BLUETOOTH_SCO);
        DEVICE_OUT_ALL_SCO_SET.add(DEVICE_OUT_BLUETOOTH_SCO_HEADSET);
        DEVICE_OUT_ALL_SCO_SET.add(DEVICE_OUT_BLUETOOTH_SCO_CARKIT);

        DEVICE_OUT_ALL_USB_SET = new HashSet<>();
        DEVICE_OUT_ALL_USB_SET.add(DEVICE_OUT_USB_ACCESSORY);
        DEVICE_OUT_ALL_USB_SET.add(DEVICE_OUT_USB_DEVICE);
        DEVICE_OUT_ALL_USB_SET.add(DEVICE_OUT_USB_HEADSET);

        DEVICE_OUT_ALL_HDMI_SYSTEM_AUDIO_SET = new HashSet<>();
        DEVICE_OUT_ALL_HDMI_SYSTEM_AUDIO_SET.add(DEVICE_OUT_AUX_LINE);
        DEVICE_OUT_ALL_HDMI_SYSTEM_AUDIO_SET.add(DEVICE_OUT_HDMI_ARC);
        DEVICE_OUT_ALL_HDMI_SYSTEM_AUDIO_SET.add(DEVICE_OUT_HDMI_EARC);
        DEVICE_OUT_ALL_HDMI_SYSTEM_AUDIO_SET.add(DEVICE_OUT_SPDIF);

        DEVICE_ALL_HDMI_SYSTEM_AUDIO_AND_SPEAKER_SET = new HashSet<>();
        DEVICE_ALL_HDMI_SYSTEM_AUDIO_AND_SPEAKER_SET.addAll(DEVICE_OUT_ALL_HDMI_SYSTEM_AUDIO_SET);
        DEVICE_ALL_HDMI_SYSTEM_AUDIO_AND_SPEAKER_SET.add(DEVICE_OUT_SPEAKER);

        DEVICE_OUT_ALL_BLE_SET = new HashSet<>();
        DEVICE_OUT_ALL_BLE_SET.add(DEVICE_OUT_BLE_HEADSET);
        DEVICE_OUT_ALL_BLE_SET.add(DEVICE_OUT_BLE_SPEAKER);
        DEVICE_OUT_ALL_BLE_SET.add(DEVICE_OUT_BLE_BROADCAST);
    }

    // input devices
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_COMMUNICATION = DEVICE_BIT_IN | 0x1;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_AMBIENT = DEVICE_BIT_IN | 0x2;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_BUILTIN_MIC = DEVICE_BIT_IN | 0x4;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_BLUETOOTH_SCO_HEADSET = DEVICE_BIT_IN | 0x8;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_WIRED_HEADSET = DEVICE_BIT_IN | 0x10;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_AUX_DIGITAL = DEVICE_BIT_IN | 0x20;
    /** @hide */
    public static final int DEVICE_IN_HDMI = DEVICE_IN_AUX_DIGITAL;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_VOICE_CALL = DEVICE_BIT_IN | 0x40;
    /** @hide */
    public static final int DEVICE_IN_TELEPHONY_RX = DEVICE_IN_VOICE_CALL;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_BACK_MIC = DEVICE_BIT_IN | 0x80;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_REMOTE_SUBMIX = DEVICE_BIT_IN | 0x100;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_ANLG_DOCK_HEADSET = DEVICE_BIT_IN | 0x200;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_DGTL_DOCK_HEADSET = DEVICE_BIT_IN | 0x400;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_USB_ACCESSORY = DEVICE_BIT_IN | 0x800;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_USB_DEVICE = DEVICE_BIT_IN | 0x1000;
    /** @hide */
    public static final int DEVICE_IN_FM_TUNER = DEVICE_BIT_IN | 0x2000;
    /** @hide */
    public static final int DEVICE_IN_TV_TUNER = DEVICE_BIT_IN | 0x4000;
    /** @hide */
    public static final int DEVICE_IN_LINE = DEVICE_BIT_IN | 0x8000;
    /** @hide */
    public static final int DEVICE_IN_SPDIF = DEVICE_BIT_IN | 0x10000;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_BLUETOOTH_A2DP = DEVICE_BIT_IN | 0x20000;
    /** @hide */
    public static final int DEVICE_IN_LOOPBACK = DEVICE_BIT_IN | 0x40000;
    /** @hide */
    public static final int DEVICE_IN_IP = DEVICE_BIT_IN | 0x80000;
    /** @hide */
    public static final int DEVICE_IN_BUS = DEVICE_BIT_IN | 0x100000;
    /** @hide */
    public static final int DEVICE_IN_PROXY = DEVICE_BIT_IN | 0x1000000;
    /** @hide */
    public static final int DEVICE_IN_USB_HEADSET = DEVICE_BIT_IN | 0x2000000;
    /** @hide */
    public static final int DEVICE_IN_BLUETOOTH_BLE = DEVICE_BIT_IN | 0x4000000;
    /** @hide */
    public static final int DEVICE_IN_HDMI_ARC = DEVICE_BIT_IN | 0x8000000;
    /** @hide */
    public static final int DEVICE_IN_HDMI_EARC = DEVICE_BIT_IN | 0x8000001;
    /** @hide */
    public static final int DEVICE_IN_ECHO_REFERENCE = DEVICE_BIT_IN | 0x10000000;
    /** @hide */
    public static final int DEVICE_IN_BLE_HEADSET = DEVICE_BIT_IN | 0x20000000;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_IN_DEFAULT = DEVICE_BIT_IN | DEVICE_BIT_DEFAULT;

    /** @hide */
    public static final Set<Integer> DEVICE_IN_ALL_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_IN_ALL_SCO_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_IN_ALL_USB_SET;
    /** @hide */
    public static final Set<Integer> DEVICE_IN_ALL_BLE_SET;

    static {
        DEVICE_IN_ALL_SET = new HashSet<>();
        DEVICE_IN_ALL_SET.add(DEVICE_IN_COMMUNICATION);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_AMBIENT);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_BUILTIN_MIC);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_BLUETOOTH_SCO_HEADSET);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_WIRED_HEADSET);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_HDMI);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_TELEPHONY_RX);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_BACK_MIC);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_REMOTE_SUBMIX);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_ANLG_DOCK_HEADSET);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_DGTL_DOCK_HEADSET);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_USB_ACCESSORY);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_USB_DEVICE);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_FM_TUNER);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_TV_TUNER);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_LINE);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_SPDIF);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_BLUETOOTH_A2DP);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_LOOPBACK);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_IP);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_BUS);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_PROXY);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_USB_HEADSET);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_BLUETOOTH_BLE);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_HDMI_ARC);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_HDMI_EARC);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_ECHO_REFERENCE);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_BLE_HEADSET);
        DEVICE_IN_ALL_SET.add(DEVICE_IN_DEFAULT);

        DEVICE_IN_ALL_SCO_SET = new HashSet<>();
        DEVICE_IN_ALL_SCO_SET.add(DEVICE_IN_BLUETOOTH_SCO_HEADSET);

        DEVICE_IN_ALL_USB_SET = new HashSet<>();
        DEVICE_IN_ALL_USB_SET.add(DEVICE_IN_USB_ACCESSORY);
        DEVICE_IN_ALL_USB_SET.add(DEVICE_IN_USB_DEVICE);
        DEVICE_IN_ALL_USB_SET.add(DEVICE_IN_USB_HEADSET);

        DEVICE_IN_ALL_BLE_SET = new HashSet<>();
        DEVICE_IN_ALL_BLE_SET.add(DEVICE_IN_BLE_HEADSET);
    }

    /** @hide */
    public static boolean isInputDevice(int deviceType) {
        return (deviceType & DEVICE_BIT_IN) == DEVICE_BIT_IN;
    }

    /** @hide */
    public static boolean isBluetoothDevice(int deviceType) {
        return isBluetoothA2dpOutDevice(deviceType)
                || isBluetoothScoDevice(deviceType)
                || isBluetoothLeDevice(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothOutDevice(int deviceType) {
        return isBluetoothA2dpOutDevice(deviceType)
                || isBluetoothScoOutDevice(deviceType)
                || isBluetoothLeOutDevice(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothInDevice(int deviceType) {
        return isBluetoothScoInDevice(deviceType)
                || isBluetoothLeInDevice(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothA2dpOutDevice(int deviceType) {
        return DEVICE_OUT_ALL_A2DP_SET.contains(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothScoOutDevice(int deviceType) {
        return DEVICE_OUT_ALL_SCO_SET.contains(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothScoInDevice(int deviceType) {
        return DEVICE_IN_ALL_SCO_SET.contains(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothScoDevice(int deviceType) {
        return isBluetoothScoOutDevice(deviceType)
                || isBluetoothScoInDevice(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothLeOutDevice(int deviceType) {
        return DEVICE_OUT_ALL_BLE_SET.contains(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothLeInDevice(int deviceType) {
        return DEVICE_IN_ALL_BLE_SET.contains(deviceType);
    }

    /** @hide */
    public static boolean isBluetoothLeDevice(int deviceType) {
        return isBluetoothLeOutDevice(deviceType)
                || isBluetoothLeInDevice(deviceType);
    }

    /** @hide */
    public static boolean isRemoteSubmixDevice(int deviceType) {
        return deviceType == DEVICE_IN_REMOTE_SUBMIX || deviceType == DEVICE_OUT_REMOTE_SUBMIX;
    }

    /** @hide */
    public static final String LEGACY_REMOTE_SUBMIX_ADDRESS = "0";

    // device states, must match AudioSystem::device_connection_state
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_STATE_UNAVAILABLE = 0;
    /** @hide */
    @UnsupportedAppUsage
    public static final int DEVICE_STATE_AVAILABLE = 1;
    private static final int NUM_DEVICE_STATES = 1;

    /** @hide */
    public static String deviceStateToString(int state) {
        switch (state) {
            case DEVICE_STATE_UNAVAILABLE: return "DEVICE_STATE_UNAVAILABLE";
            case DEVICE_STATE_AVAILABLE: return "DEVICE_STATE_AVAILABLE";
            default: return "unknown state (" + state + ")";
        }
    }

    /** @hide */ public static final String DEVICE_OUT_EARPIECE_NAME = "earpiece";
    /** @hide */ public static final String DEVICE_OUT_SPEAKER_NAME = "speaker";
    /** @hide */ public static final String DEVICE_OUT_WIRED_HEADSET_NAME = "headset";
    /** @hide */ public static final String DEVICE_OUT_WIRED_HEADPHONE_NAME = "headphone";
    /** @hide */ public static final String DEVICE_OUT_BLUETOOTH_SCO_NAME = "bt_sco";
    /** @hide */ public static final String DEVICE_OUT_BLUETOOTH_SCO_HEADSET_NAME = "bt_sco_hs";
    /** @hide */ public static final String DEVICE_OUT_BLUETOOTH_SCO_CARKIT_NAME = "bt_sco_carkit";
    /** @hide */ public static final String DEVICE_OUT_BLUETOOTH_A2DP_NAME = "bt_a2dp";
    /** @hide */
    public static final String DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES_NAME = "bt_a2dp_hp";
    /** @hide */ public static final String DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER_NAME = "bt_a2dp_spk";
    /** @hide */ public static final String DEVICE_OUT_AUX_DIGITAL_NAME = "aux_digital";
    /** @hide */ public static final String DEVICE_OUT_HDMI_NAME = "hdmi";
    /** @hide */ public static final String DEVICE_OUT_ANLG_DOCK_HEADSET_NAME = "analog_dock";
    /** @hide */ public static final String DEVICE_OUT_DGTL_DOCK_HEADSET_NAME = "digital_dock";
    /** @hide */ public static final String DEVICE_OUT_USB_ACCESSORY_NAME = "usb_accessory";
    /** @hide */ public static final String DEVICE_OUT_USB_DEVICE_NAME = "usb_device";
    /** @hide */ public static final String DEVICE_OUT_REMOTE_SUBMIX_NAME = "remote_submix";
    /** @hide */ public static final String DEVICE_OUT_TELEPHONY_TX_NAME = "telephony_tx";
    /** @hide */ public static final String DEVICE_OUT_LINE_NAME = "line";
    /** @hide */ public static final String DEVICE_OUT_HDMI_ARC_NAME = "hdmi_arc";
    /** @hide */ public static final String DEVICE_OUT_HDMI_EARC_NAME = "hdmi_earc";
    /** @hide */ public static final String DEVICE_OUT_SPDIF_NAME = "spdif";
    /** @hide */ public static final String DEVICE_OUT_FM_NAME = "fm_transmitter";
    /** @hide */ public static final String DEVICE_OUT_AUX_LINE_NAME = "aux_line";
    /** @hide */ public static final String DEVICE_OUT_SPEAKER_SAFE_NAME = "speaker_safe";
    /** @hide */ public static final String DEVICE_OUT_IP_NAME = "ip";
    /** @hide */ public static final String DEVICE_OUT_BUS_NAME = "bus";
    /** @hide */ public static final String DEVICE_OUT_PROXY_NAME = "proxy";
    /** @hide */ public static final String DEVICE_OUT_USB_HEADSET_NAME = "usb_headset";
    /** @hide */ public static final String DEVICE_OUT_HEARING_AID_NAME = "hearing_aid_out";
    /** @hide */ public static final String DEVICE_OUT_ECHO_CANCELLER_NAME = "echo_canceller";
    /** @hide */ public static final String DEVICE_OUT_BLE_HEADSET_NAME = "ble_headset";
    /** @hide */ public static final String DEVICE_OUT_BLE_SPEAKER_NAME = "ble_speaker";
    /** @hide */ public static final String DEVICE_OUT_BLE_BROADCAST_NAME = "ble_broadcast";

    /** @hide */ public static final String DEVICE_IN_COMMUNICATION_NAME = "communication";
    /** @hide */ public static final String DEVICE_IN_AMBIENT_NAME = "ambient";
    /** @hide */ public static final String DEVICE_IN_BUILTIN_MIC_NAME = "mic";
    /** @hide */ public static final String DEVICE_IN_BLUETOOTH_SCO_HEADSET_NAME = "bt_sco_hs";
    /** @hide */ public static final String DEVICE_IN_WIRED_HEADSET_NAME = "headset";
    /** @hide */ public static final String DEVICE_IN_AUX_DIGITAL_NAME = "aux_digital";
    /** @hide */ public static final String DEVICE_IN_TELEPHONY_RX_NAME = "telephony_rx";
    /** @hide */ public static final String DEVICE_IN_BACK_MIC_NAME = "back_mic";
    /** @hide */ public static final String DEVICE_IN_REMOTE_SUBMIX_NAME = "remote_submix";
    /** @hide */ public static final String DEVICE_IN_ANLG_DOCK_HEADSET_NAME = "analog_dock";
    /** @hide */ public static final String DEVICE_IN_DGTL_DOCK_HEADSET_NAME = "digital_dock";
    /** @hide */ public static final String DEVICE_IN_USB_ACCESSORY_NAME = "usb_accessory";
    /** @hide */ public static final String DEVICE_IN_USB_DEVICE_NAME = "usb_device";
    /** @hide */ public static final String DEVICE_IN_FM_TUNER_NAME = "fm_tuner";
    /** @hide */ public static final String DEVICE_IN_TV_TUNER_NAME = "tv_tuner";
    /** @hide */ public static final String DEVICE_IN_LINE_NAME = "line";
    /** @hide */ public static final String DEVICE_IN_SPDIF_NAME = "spdif";
    /** @hide */ public static final String DEVICE_IN_BLUETOOTH_A2DP_NAME = "bt_a2dp";
    /** @hide */ public static final String DEVICE_IN_LOOPBACK_NAME = "loopback";
    /** @hide */ public static final String DEVICE_IN_IP_NAME = "ip";
    /** @hide */ public static final String DEVICE_IN_BUS_NAME = "bus";
    /** @hide */ public static final String DEVICE_IN_PROXY_NAME = "proxy";
    /** @hide */ public static final String DEVICE_IN_USB_HEADSET_NAME = "usb_headset";
    /** @hide */ public static final String DEVICE_IN_BLUETOOTH_BLE_NAME = "bt_ble";
    /** @hide */ public static final String DEVICE_IN_ECHO_REFERENCE_NAME = "echo_reference";
    /** @hide */ public static final String DEVICE_IN_HDMI_ARC_NAME = "hdmi_arc";
    /** @hide */ public static final String DEVICE_IN_HDMI_EARC_NAME = "hdmi_earc";
    /** @hide */ public static final String DEVICE_IN_BLE_HEADSET_NAME = "ble_headset";

    /** @hide */
    @UnsupportedAppUsage
    public static String getOutputDeviceName(int device)
    {
        switch(device) {
        case DEVICE_OUT_EARPIECE:
            return DEVICE_OUT_EARPIECE_NAME;
        case DEVICE_OUT_SPEAKER:
            return DEVICE_OUT_SPEAKER_NAME;
        case DEVICE_OUT_WIRED_HEADSET:
            return DEVICE_OUT_WIRED_HEADSET_NAME;
        case DEVICE_OUT_WIRED_HEADPHONE:
            return DEVICE_OUT_WIRED_HEADPHONE_NAME;
        case DEVICE_OUT_BLUETOOTH_SCO:
            return DEVICE_OUT_BLUETOOTH_SCO_NAME;
        case DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            return DEVICE_OUT_BLUETOOTH_SCO_HEADSET_NAME;
        case DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            return DEVICE_OUT_BLUETOOTH_SCO_CARKIT_NAME;
        case DEVICE_OUT_BLUETOOTH_A2DP:
            return DEVICE_OUT_BLUETOOTH_A2DP_NAME;
        case DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
            return DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES_NAME;
        case DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER:
            return DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER_NAME;
        case DEVICE_OUT_HDMI:
            return DEVICE_OUT_HDMI_NAME;
        case DEVICE_OUT_ANLG_DOCK_HEADSET:
            return DEVICE_OUT_ANLG_DOCK_HEADSET_NAME;
        case DEVICE_OUT_DGTL_DOCK_HEADSET:
            return DEVICE_OUT_DGTL_DOCK_HEADSET_NAME;
        case DEVICE_OUT_USB_ACCESSORY:
            return DEVICE_OUT_USB_ACCESSORY_NAME;
        case DEVICE_OUT_USB_DEVICE:
            return DEVICE_OUT_USB_DEVICE_NAME;
        case DEVICE_OUT_REMOTE_SUBMIX:
            return DEVICE_OUT_REMOTE_SUBMIX_NAME;
        case DEVICE_OUT_TELEPHONY_TX:
            return DEVICE_OUT_TELEPHONY_TX_NAME;
        case DEVICE_OUT_LINE:
            return DEVICE_OUT_LINE_NAME;
        case DEVICE_OUT_HDMI_ARC:
            return DEVICE_OUT_HDMI_ARC_NAME;
        case DEVICE_OUT_HDMI_EARC:
            return DEVICE_OUT_HDMI_EARC_NAME;
        case DEVICE_OUT_SPDIF:
            return DEVICE_OUT_SPDIF_NAME;
        case DEVICE_OUT_FM:
            return DEVICE_OUT_FM_NAME;
        case DEVICE_OUT_AUX_LINE:
            return DEVICE_OUT_AUX_LINE_NAME;
        case DEVICE_OUT_SPEAKER_SAFE:
            return DEVICE_OUT_SPEAKER_SAFE_NAME;
        case DEVICE_OUT_IP:
            return DEVICE_OUT_IP_NAME;
        case DEVICE_OUT_BUS:
            return DEVICE_OUT_BUS_NAME;
        case DEVICE_OUT_PROXY:
            return DEVICE_OUT_PROXY_NAME;
        case DEVICE_OUT_USB_HEADSET:
            return DEVICE_OUT_USB_HEADSET_NAME;
        case DEVICE_OUT_HEARING_AID:
            return DEVICE_OUT_HEARING_AID_NAME;
        case DEVICE_OUT_ECHO_CANCELLER:
            return DEVICE_OUT_ECHO_CANCELLER_NAME;
        case DEVICE_OUT_BLE_HEADSET:
            return DEVICE_OUT_BLE_HEADSET_NAME;
        case DEVICE_OUT_BLE_SPEAKER:
            return DEVICE_OUT_BLE_SPEAKER_NAME;
        case DEVICE_OUT_BLE_BROADCAST:
            return DEVICE_OUT_BLE_BROADCAST_NAME;
        case DEVICE_OUT_DEFAULT:
        default:
            return "0x" + Integer.toHexString(device);
        }
    }

    /** @hide */
    public static String getInputDeviceName(int device)
    {
        switch(device) {
        case DEVICE_IN_COMMUNICATION:
            return DEVICE_IN_COMMUNICATION_NAME;
        case DEVICE_IN_AMBIENT:
            return DEVICE_IN_AMBIENT_NAME;
        case DEVICE_IN_BUILTIN_MIC:
            return DEVICE_IN_BUILTIN_MIC_NAME;
        case DEVICE_IN_BLUETOOTH_SCO_HEADSET:
            return DEVICE_IN_BLUETOOTH_SCO_HEADSET_NAME;
        case DEVICE_IN_WIRED_HEADSET:
            return DEVICE_IN_WIRED_HEADSET_NAME;
        case DEVICE_IN_AUX_DIGITAL:
            return DEVICE_IN_AUX_DIGITAL_NAME;
        case DEVICE_IN_TELEPHONY_RX:
            return DEVICE_IN_TELEPHONY_RX_NAME;
        case DEVICE_IN_BACK_MIC:
            return DEVICE_IN_BACK_MIC_NAME;
        case DEVICE_IN_REMOTE_SUBMIX:
            return DEVICE_IN_REMOTE_SUBMIX_NAME;
        case DEVICE_IN_ANLG_DOCK_HEADSET:
            return DEVICE_IN_ANLG_DOCK_HEADSET_NAME;
        case DEVICE_IN_DGTL_DOCK_HEADSET:
            return DEVICE_IN_DGTL_DOCK_HEADSET_NAME;
        case DEVICE_IN_USB_ACCESSORY:
            return DEVICE_IN_USB_ACCESSORY_NAME;
        case DEVICE_IN_USB_DEVICE:
            return DEVICE_IN_USB_DEVICE_NAME;
        case DEVICE_IN_FM_TUNER:
            return DEVICE_IN_FM_TUNER_NAME;
        case DEVICE_IN_TV_TUNER:
            return DEVICE_IN_TV_TUNER_NAME;
        case DEVICE_IN_LINE:
            return DEVICE_IN_LINE_NAME;
        case DEVICE_IN_SPDIF:
            return DEVICE_IN_SPDIF_NAME;
        case DEVICE_IN_BLUETOOTH_A2DP:
            return DEVICE_IN_BLUETOOTH_A2DP_NAME;
        case DEVICE_IN_LOOPBACK:
            return DEVICE_IN_LOOPBACK_NAME;
        case DEVICE_IN_IP:
            return DEVICE_IN_IP_NAME;
        case DEVICE_IN_BUS:
            return DEVICE_IN_BUS_NAME;
        case DEVICE_IN_PROXY:
            return DEVICE_IN_PROXY_NAME;
        case DEVICE_IN_USB_HEADSET:
            return DEVICE_IN_USB_HEADSET_NAME;
        case DEVICE_IN_BLUETOOTH_BLE:
            return DEVICE_IN_BLUETOOTH_BLE_NAME;
        case DEVICE_IN_ECHO_REFERENCE:
            return DEVICE_IN_ECHO_REFERENCE_NAME;
        case DEVICE_IN_HDMI_ARC:
            return DEVICE_IN_HDMI_ARC_NAME;
        case DEVICE_IN_HDMI_EARC:
            return DEVICE_IN_HDMI_EARC_NAME;
        case DEVICE_IN_BLE_HEADSET:
            return DEVICE_IN_BLE_HEADSET_NAME;
        case DEVICE_IN_DEFAULT:
        default:
            return Integer.toString(device);
        }
    }

    /**
     * @hide
     * Returns a human readable name for a given device type
     * @param device a native device type, NOT an AudioDeviceInfo type
     * @return a string describing the device type
     */
    public static @NonNull String getDeviceName(int device) {
        if (isInputDevice(device)) {
            return getInputDeviceName(device);
        }
        return getOutputDeviceName(device);
    }
}
