/*
 * Ours under the generated class's name (ADR-0045): the usages of the audio policy configuration's
 * schema (version 7.0), which xsdc generates from audio_policy_configuration.xsd. AudioAttributes maps its usages to
 * these names and back in its class initializer, so the class must be there; Minato reads no audio policy file. Each
 * constant's raw name is its name, as in the schema.
 */
package android.audio.policy.configuration.V7_0;

/** @hide */
public enum AudioUsage {
    AUDIO_USAGE_UNKNOWN,
    AUDIO_USAGE_MEDIA,
    AUDIO_USAGE_VOICE_COMMUNICATION,
    AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING,
    AUDIO_USAGE_ALARM,
    AUDIO_USAGE_NOTIFICATION,
    AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE,
    AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY,
    AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE,
    AUDIO_USAGE_ASSISTANCE_SONIFICATION,
    AUDIO_USAGE_GAME,
    AUDIO_USAGE_VIRTUAL_SOURCE,
    AUDIO_USAGE_ASSISTANT,
    AUDIO_USAGE_CALL_ASSISTANT,
    AUDIO_USAGE_EMERGENCY,
    AUDIO_USAGE_SAFETY,
    AUDIO_USAGE_VEHICLE_STATUS,
    AUDIO_USAGE_ANNOUNCEMENT;

    /** The name in the schema. */
    public String getRawName() {
        return name();
    }
}
