// Hidden members of the public API's compile-only stub (ADR-0039 step c2c3), which tools/managergen joins with it:
// Activity's voice interaction events.
class VoiceInteractionSession {
    /** @hide */
    public static final int VOICE_INTERACTION_ACTIVITY_EVENT_START = 1;

    /** @hide */
    public static final int VOICE_INTERACTION_ACTIVITY_EVENT_RESUME = 2;

    /** @hide */
    public static final int VOICE_INTERACTION_ACTIVITY_EVENT_PAUSE = 3;

    /** @hide */
    public static final int VOICE_INTERACTION_ACTIVITY_EVENT_STOP = 4;

    /** @hide */
    public @interface VoiceInteractionActivityEventType {
    }
}
