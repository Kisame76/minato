/*
 * Ours under AOSP's name (ADR-0045 gap 1, batch 2): the audio policy's product strategies, which AudioAttributes asks
 * only to turn a legacy stream type into attributes and back. Minato has no audio policy service, so the list is
 * empty, as a device without one would give it, and AudioAttributes then maps stream types and usages itself, as
 * AOSP's does for an empty list.
 */
package android.media.audiopolicy;

import android.media.AudioAttributes;
import android.media.AudioManager;

import java.util.Collections;
import java.util.List;

/** @hide */
public final class AudioProductStrategy {
    private AudioProductStrategy() {
    }

    /** The strategies of the platform's audio policy: none. */
    public static List<AudioProductStrategy> getAudioProductStrategies() {
        return Collections.emptyList();
    }

    /** AOSP's answer when no strategy knows the stream type: the default attributes. */
    public static AudioAttributes getAudioAttributesForStrategyWithLegacyStreamType(int streamType) {
        return new AudioAttributes.Builder().build();
    }

    /** AOSP's answer when no strategy knows the attributes: STREAM_MUSIC. */
    public static int getLegacyStreamTypeForStrategyWithAudioAttributes(AudioAttributes audioAttributes) {
        return AudioManager.STREAM_MUSIC;
    }
}
