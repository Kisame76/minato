package android.graphics;

/**
 * Compile-only stub (ADR-0043 test 2), never in framework.jar: RecordingCanvas draws a TextureView's layer with it,
 * and HardwareRenderer makes, copies and updates one, which the GPU pipeline does. framework.jar leaves the pipeline
 * out (decision 2).
 */
public final class TextureLayer {
    static TextureLayer adoptTextureLayer(HardwareRenderer renderer, long layer) {
        throw new RuntimeException("stub");
    }

    long getDeferredLayerUpdater() {
        throw new RuntimeException("stub");
    }

    long getLayerHandle() {
        throw new RuntimeException("stub");
    }
}
