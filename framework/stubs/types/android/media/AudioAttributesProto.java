package android.media;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the proto of
 * core/proto/android/media/audioattributes.proto, whose field IDs AudioAttributes.dumpDebug writes; javac inlines
 * them. Each ID is ProtoStream's: count single (1 << 40) or repeated (2 << 40), the field's type << 32, its number.
 */
public final class AudioAttributesProto {
    public static final long USAGE = 0x10e00000001L;
    public static final long CONTENT_TYPE = 0x10e00000002L;
    public static final long FLAGS = 0x10500000003L;
    public static final long TAGS = 0x20900000004L;
}
