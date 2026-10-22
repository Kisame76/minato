package android.media.codec;

/**
 * Minato's android.media.codec.Flags (ADR-0060 e2b3), in place of the class aconfig generates from frameworks/av's
 * declarations, which are not fetched: the flag names that Android 15's public API (core/api/current.txt) gives in
 * @FlaggedApi, and each flag's value for what MediaCodec and MediaCodecInfo ask at run time: Android 15's, as
 * media-codec-test reads them on the emulator by reflection (its device tag).
 * @hide
 */
public final class Flags {
    public static final String FLAG_DYNAMIC_COLOR_ASPECTS = "android.media.codec.dynamic_color_aspects";
    public static final String FLAG_HLG_EDITING = "android.media.codec.hlg_editing";
    public static final String FLAG_IN_PROCESS_SW_AUDIO_CODEC = "android.media.codec.in_process_sw_audio_codec";
    public static final String FLAG_NULL_OUTPUT_SURFACE = "android.media.codec.null_output_surface";
    public static final String FLAG_REGION_OF_INTEREST = "android.media.codec.region_of_interest";

    private Flags() {
    }

    public static boolean dynamicColorAspects() {
        return true;
    }

    public static boolean hlgEditing() {
        return true;
    }

    public static boolean inProcessSwAudioCodec() {
        return false;
    }

    public static boolean nullOutputSurface() {
        return true;
    }

    public static boolean nullOutputSurfaceSupport() {
        return true;
    }

    public static boolean regionOfInterest() {
        return true;
    }

    public static boolean setCallbackStall() {
        return false;
    }
}
