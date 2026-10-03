package android.graphics;

/**
 * Compile-only stub (ADR-0043 test 2), never in framework.jar: RecordingCanvas draws a TextureView's layer with it,
 * which the GPU pipeline makes. framework.jar leaves the pipeline out (decision 2).
 */
public final class TextureLayer {
    long getLayerHandle() {
        throw new RuntimeException("stub");
    }
}
