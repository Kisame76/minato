// The written part of AudioManager (ADR-0045 decision 3), which tools/managergen joins with the API's methods: the
// hidden ctor SystemServiceRegistry calls, and decision 4's answers that need no iOS (gap 1 of the compatibility plan,
// batch 1). Focus is granted at once and given back; the mode, ringer and volumes are those of a phone at rest, read
// only: each stream at AudioSystem.DEFAULT_STREAM_VOLUME between AudioService's MIN_ and MAX_STREAM_VOLUME at
// android-15.0.0_r1 [read], which apk-runner-35-bare shows for music [verified: services-test]. The output's rate
// and burst are AAudio's usual ones until step c reads the session's (a named difference): an engine that sizes its
// buffers from them must get numbers, never null. Interruptions, focus losses and the route come with step c.
import android.annotation.NonNull;
import android.annotation.Nullable;
import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.util.Log;

import java.io.IOException;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;

class AudioManager {
    private static final int[] DEFAULT_STREAM_VOLUME = {4, 7, 5, 5, 6, 5, 7, 7, 5, 5, 5, 5};
    private static final int[] MIN_STREAM_VOLUME = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    private static final int[] MAX_STREAM_VOLUME = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
    private static final String OUTPUT_SAMPLE_RATE = "48000";
    private static final String OUTPUT_FRAMES_PER_BUFFER = "256";

    private final Context context;
    /** AOSP's: the context of the last manager made, which hasHapticChannels takes without one. */
    private static WeakReference<Context> sContext = new WeakReference<>(null);

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's AudioManager keeps its
     * context as well.
     */
    public AudioManager(Context context) {
        this.context = context;
        sContext = new WeakReference<>(context);
    }

    public int requestAudioFocus(OnAudioFocusChangeListener l, int streamType, int durationHint) {
        return AUDIOFOCUS_REQUEST_GRANTED;
    }

    public int abandonAudioFocus(OnAudioFocusChangeListener l) {
        return AUDIOFOCUS_REQUEST_GRANTED;
    }

    public int getMode() {
        return MODE_NORMAL;
    }

    public int getRingerMode() {
        return RINGER_MODE_NORMAL;
    }

    public boolean isMusicActive() {
        return false;
    }

    public boolean isSpeakerphoneOn() {
        return false;
    }

    public boolean isMicrophoneMute() {
        return false;
    }

    public boolean isVolumeFixed() {
        return false;
    }

    /**
     * The session's output, as a phone's built-in speaker (gap 13: FMOD lists the outputs before it builds its
     * AudioTrack, and Android lists the speaker among them); no input, as Minato records nothing yet (a named
     * difference: Android lists the built-in microphone).
     */
    public AudioDeviceInfo[] getDevices(int flags) {
        return devices(flags);
    }

    private static AudioDeviceInfo[] devices(int flags) {
        if ((flags & GET_DEVICES_OUTPUTS) == 0) {
            return new AudioDeviceInfo[0];
        }
        return new AudioDeviceInfo[] {new AudioDeviceInfo(BUILTIN_SPEAKER_ID, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER,
                true)};
    }

    /** @hide AudioSystem's status for success, as AOSP's AudioManager repeats it. */
    public static final int SUCCESS = AudioSystem.SUCCESS;

    /** @hide Whether a native device type is an input, as AOSP's. */
    public static boolean isInputDevice(int device) {
        return AudioSystem.isInputDevice(device);
    }

    /**
     * @hide The connections between audio ports. Minato has no audio server and so no patches (a named difference:
     * Android lists the patches of the phone's audio paths); the call succeeds with an empty list, as on a phone
     * with none.
     */
    public static int listAudioPatches(ArrayList<AudioPatch> patches) {
        patches.clear();
        return SUCCESS;
    }

    /** @hide As AOSP's: the device whose port has this ID among those the flags select, else null. */
    public static AudioDeviceInfo getDeviceForPortId(int portId, int flags) {
        if (portId == 0) {
            return null;
        }
        for (AudioDeviceInfo device : devices(flags)) {
            if (device.getId() == portId) {
                return device;
            }
        }
        return null;
    }

    private final List<AudioRecordingCallback> recordingCallbacks = new ArrayList<>();

    /**
     * Callbacks are kept, as AOSP keeps its list; none is called until the microphone exists (ADR-0064), since
     * nothing records before. A callback registered twice is logged and kept once, as AOSP does.
     */
    public void registerAudioRecordingCallback(AudioRecordingCallback cb, Handler handler) {
        if (cb == null) {
            throw new IllegalArgumentException("Illegal null AudioRecordingCallback argument");
        }
        synchronized (recordingCallbacks) {
            if (recordingCallbacks.contains(cb)) {
                Log.w(TAG, "attempt to call registerAudioRecordingCallback() on a previously"
                        + "registered callback");
            } else {
                recordingCallbacks.add(cb);
            }
        }
    }

    public void unregisterAudioRecordingCallback(AudioRecordingCallback cb) {
        if (cb == null) {
            throw new IllegalArgumentException("Illegal null AudioRecordingCallback argument");
        }
        synchronized (recordingCallbacks) {
            if (!recordingCallbacks.remove(cb)) {
                Log.w(TAG, "attempt to call unregisterAudioRecordingCallback() on a callback"
                        + " already unregistered or never registered");
            }
        }
    }

    /** No recording is active: the list is empty, as AOSP's says an empty list means. */
    public List<AudioRecordingConfiguration> getActiveRecordingConfigurations() {
        return new ArrayList<>();
    }

    private static final String TAG = "AudioManager";

    /** The speaker's port ID, as the audio server numbers its ports from 1. */
    private static final int BUILTIN_SPEAKER_ID = 2;

    public int getStreamVolume(int streamType) {
        return volume(DEFAULT_STREAM_VOLUME, streamType);
    }

    public int getStreamMinVolume(int streamType) {
        return volume(MIN_STREAM_VOLUME, streamType);
    }

    public int getStreamMaxVolume(int streamType) {
        return volume(MAX_STREAM_VOLUME, streamType);
    }

    public String getProperty(String key) {
        if (PROPERTY_OUTPUT_SAMPLE_RATE.equals(key)) {
            return OUTPUT_SAMPLE_RATE;
        }
        if (PROPERTY_OUTPUT_FRAMES_PER_BUFFER.equals(key)) {
            return OUTPUT_FRAMES_PER_BUFFER;
        }
        return null;
    }

    /** @hide As AOSP's: the focus request flags that AudioFocusRequest names (android.media's take-in classes). */
    public static final int AUDIOFOCUS_FLAG_DELAY_OK = 0x1 << 0;

    /** @hide As AOSP's. */
    public static final int AUDIOFOCUS_FLAG_PAUSES_ON_DUCKABLE_LOSS = 0x1 << 1;

    /** @hide As AOSP's. */
    public static final int AUDIOFOCUS_FLAG_LOCK = 0x1 << 2;

    /** @hide As AOSP's: the sounds of a repeated focus move, which SoundEffectConstants picks (ADR-0045 step 7c1). */
    public static final int NUM_NAVIGATION_REPEAT_SOUND_EFFECTS = 4;

    /** @hide As AOSP's. */
    public static final int FX_FOCUS_NAVIGATION_REPEAT_1 = 12;

    /** @hide As AOSP's. */
    public static final int FX_FOCUS_NAVIGATION_REPEAT_2 = 13;

    /** @hide As AOSP's. */
    public static final int FX_FOCUS_NAVIGATION_REPEAT_3 = 14;

    /** @hide As AOSP's. */
    public static final int FX_FOCUS_NAVIGATION_REPEAT_4 = 15;

    /** @hide As AOSP's: the nth of those sounds, or -1 out of range (AOSP logs a warning there). */
    public static int getNthNavigationRepeatSoundEffect(int n) {
        switch (n) {
            case 0:
                return FX_FOCUS_NAVIGATION_REPEAT_1;
            case 1:
                return FX_FOCUS_NAVIGATION_REPEAT_2;
            case 2:
                return FX_FOCUS_NAVIGATION_REPEAT_3;
            case 3:
                return FX_FOCUS_NAVIGATION_REPEAT_4;
            default:
                return -1;
        }
    }

    /**
     * @hide Ringtone's remote player (ADR-0060 e2e1's RingtoneManager step): the audio service hands out SystemUI's,
     * here the process's own (LocalRingtonePlayer, SystemUI's port).
     */
    public IRingtonePlayer getRingtonePlayer() {
        return LocalRingtonePlayer.get(context);
    }

    /**
     * @hide As AOSP's, which Ringtone asks (the RingtoneManager step): whether a sound has haptic channels, by its
     * extractor's formats; without a context and a manager made before, Android asks the audio service, which is not
     * here: none then.
     */
    public static boolean hasHapticChannelsImpl(@NonNull Context context, @NonNull Uri uri) {
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(context, uri, null);
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat format = extractor.getTrackFormat(i);
                if (format.containsKey(MediaFormat.KEY_HAPTIC_CHANNEL_COUNT)
                        && format.getInteger(MediaFormat.KEY_HAPTIC_CHANNEL_COUNT) > 0) {
                    return true;
                }
            }
        } catch (IOException e) {
            Log.e(TAG, "hasHapticChannels failure:" + e);
        }
        return false;
    }

    /** @hide As AOSP's, but for the audio service (above). */
    public static boolean hasHapticChannels(@Nullable Context context, @NonNull Uri uri) {
        java.util.Objects.requireNonNull(uri);

        if (context != null) {
            return hasHapticChannelsImpl(context, uri);
        }

        Context cachedContext = sContext.get();
        if (cachedContext != null) {
            return hasHapticChannelsImpl(cachedContext, uri);
        }
        return false;
    }

    /** A stream's entry; AudioService throws IllegalArgumentException for an invalid stream type, as here. */
    /**
     * @hide MediaPlayer's and AudioTrack's routing change (ADR-0060 e2e1): Android's port generation, which no audio
     * port list here keeps, so it is always AUDIOPORT_GENERATION_INIT's 0.
     */
    static int resetAudioPortGeneration() {
        return 0;
    }

    private static int volume(int[] table, int streamType) {
        if (streamType < 0 || streamType >= table.length) {
            throw new IllegalArgumentException("Bad stream type " + streamType);
        }
        return table[streamType];
    }
}
