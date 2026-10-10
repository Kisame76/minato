package android.media;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: the native audio system's stream types, which
 * AudioAttributes maps to and from usages; javac inlines them, with their values at android-15.0.0_r1. The limits that
 * AudioFormat and our AudioTrack check (gap 13) are constants here: Android 15's natives return them (4000 and 192000
 * Hz, FCC_LIMIT 28 channels, android_media_AudioSystem.cpp at android-15.0.0_r1 [read]).
 */
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
}
