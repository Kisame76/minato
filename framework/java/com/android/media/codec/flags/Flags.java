package com.android.media.codec.flags;

/**
 * Minato's com.android.media.codec.flags.Flags (ADR-0060 e2b3), in place of the class aconfig generates from
 * frameworks/av's declarations, which are not fetched: the flag names that Android 15's public API
 * (core/api/current.txt) gives in @FlaggedApi.
 * @hide
 */
public final class Flags {
    public static final String FLAG_CODEC_IMPORTANCE = "com.android.media.codec.flags.codec_importance";
    public static final String FLAG_LARGE_AUDIO_FRAME = "com.android.media.codec.flags.large_audio_frame";

    private Flags() {
    }
}
