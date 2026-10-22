package android.sysprop;

import java.util.Optional;

/**
 * Minato's android.sysprop.MediaProperties (ADR-0060 e2b3), in place of the class sysprop generates from frameworks/av's
 * MediaProperties.sysprop, which is not fetched: the one property MediaCodecInfo reads, the largest video size a
 * 32-bit process decodes, unset, as on a device without it (MediaCodecInfo takes 4096 then). Minato's processes are
 * 64-bit, where MediaCodecInfo does not ask.
 * @hide
 */
public final class MediaProperties {
    private MediaProperties() {
    }

    public static Optional<Integer> resolution_limit_32bit() {
        return Optional.empty();
    }
}
