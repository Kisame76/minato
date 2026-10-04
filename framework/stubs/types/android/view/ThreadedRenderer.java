package android.view;

import android.graphics.Bitmap;
import android.graphics.HardwareRenderer;
import android.graphics.RenderNode;

/**
 * Compile-only stub (ADR-0043 test 2), never in framework.jar: Bitmap.createBitmap from a Picture with HARDWARE asks
 * it for HardwareRenderer's hidden createHardwareBitmap. The views draw in software (decision 2), and framework.jar
 * leaves HardwareRenderer, the GPU pipeline, out. View builds layers and observes frame metrics with it (ADR-0039
 * step c2c2).
 */
public class ThreadedRenderer extends HardwareRenderer {
    public static Bitmap createHardwareBitmap(RenderNode node, int width, int height) {
        throw new RuntimeException("stub");
    }

    public void buildLayer(RenderNode node) {
        throw new RuntimeException("stub");
    }

    public void addObserver(android.graphics.HardwareRendererObserver observer) {
        throw new RuntimeException("stub");
    }

    public void removeObserver(android.graphics.HardwareRendererObserver observer) {
        throw new RuntimeException("stub");
    }
}
