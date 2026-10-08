// The written part of AudioManager (ADR-0045 decision 3), which tools/managergen joins with the API's methods: the
// hidden ctor SystemServiceRegistry calls, and decision 4's answers that need no iOS (gap 1 of the compatibility plan,
// batch 1). Focus is granted at once and given back; the mode, ringer and volumes are those of a phone at rest, read
// only: each stream at AudioSystem.DEFAULT_STREAM_VOLUME between AudioService's MIN_ and MAX_STREAM_VOLUME at
// android-15.0.0_r1 [read], which apk-runner-35-bare shows for music [verified: services-test]. The output's rate
// and burst are AAudio's usual ones until step c reads the session's (a named difference): an engine that sizes its
// buffers from them must get numbers, never null. Interruptions, focus losses and the route come with step c.
import android.content.Context;

class AudioManager {
    private static final int[] DEFAULT_STREAM_VOLUME = {4, 7, 5, 5, 6, 5, 7, 7, 5, 5, 5, 5};
    private static final int[] MIN_STREAM_VOLUME = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};
    private static final int[] MAX_STREAM_VOLUME = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
    private static final String OUTPUT_SAMPLE_RATE = "48000";
    private static final String OUTPUT_FRAMES_PER_BUFFER = "256";

    private final Context context;

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's AudioManager keeps its
     * context as well.
     */
    public AudioManager(Context context) {
        this.context = context;
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

    /** A stream's entry; AudioService throws IllegalArgumentException for an invalid stream type, as here. */
    private static int volume(int[] table, int streamType) {
        if (streamType < 0 || streamType >= table.length) {
            throw new IllegalArgumentException("Bad stream type " + streamType);
        }
        return table[streamType];
    }
}
