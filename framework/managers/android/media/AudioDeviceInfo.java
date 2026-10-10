// The written part of AudioDeviceInfo (gap 13, the Unity test APK probe: FMOD lists the outputs before it builds its
// AudioTrack), which tools/managergen joins with the API's methods. Android's wraps an audio port of the audio server;
// ours is a device AudioManager.getDevices makes: its type and ID, a sink or a source. The product name of a built-in
// device is Build.MODEL's property, as AOSP's getProductName gives Build.MODEL for a port without a name [from
// memory]; Build itself is not named, as its annotations stop tools/managers-check.sh's compile. Empty lists of
// rates, masks, counts and encodings mean that the device takes any, as the API says.
import android.os.SystemProperties;

import java.util.ArrayList;
import java.util.List;

class AudioDeviceInfo {
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
}
