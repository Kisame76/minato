// Hidden members of the public API's compile-only stub (ADR-0045 gap 1, batch 2), which tools/managergen joins with
// it: AudioAttributes names the capture presets of the system's sources and their names for its log.
class MediaRecorder {
    public final class AudioSource {
        /** @hide */
        public static final int AUDIO_SOURCE_INVALID = -1;

        /** @hide */
        public static final int ECHO_REFERENCE = 1997;

        /** @hide */
        public static final int RADIO_TUNER = 1998;

        /** @hide */
        public static final int HOTWORD = 1999;

        /** @hide */
        public static final int ULTRASOUND = 2000;
    }

    /** @hide */
    public static final String toLogFriendlyAudioSource(int source) {
        throw new RuntimeException("stub");
    }
}
