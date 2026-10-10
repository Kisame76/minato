package android.view;

import android.graphics.Bitmap;
import android.graphics.HardwareRenderer;
import android.graphics.RenderNode;
import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the Unity test APK retest 11; Grafik owns it, replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0043 test 2): Bitmap.createBitmap from a Picture with
 * HARDWARE asks it for HardwareRenderer's hidden createHardwareBitmap. The views draw in software (decision 2), and
 * framework.jar leaves HardwareRenderer, the GPU pipeline, out. View builds layers and observes frame metrics with it
 * (ADR-0039 step c2c2).
 */
public class ThreadedRenderer extends HardwareRenderer {
    private ThreadedRenderer() {
        throw Unwritten.placeholder("android.view.ThreadedRenderer()");
    }

    /** Choreographer reads the divisor's property by it (ADR-0041 a)); javac inlines AOSP's value. */
    public static final String DEBUG_FPS_DIVISOR = "debug.hwui.fps_divisor";

    public static Bitmap createHardwareBitmap(RenderNode node, int width, int height) {
        throw Unwritten.placeholder("android.view.ThreadedRenderer.createHardwareBitmap(RenderNode,int,int)");
    }

    public void buildLayer(RenderNode node) {
        throw Unwritten.placeholder("android.view.ThreadedRenderer.buildLayer(RenderNode)");
    }

    public void addObserver(android.graphics.HardwareRendererObserver observer) {
        throw Unwritten.placeholder(
                "android.view.ThreadedRenderer.addObserver(android.graphics.HardwareRendererObserver)");
    }

    public void removeObserver(android.graphics.HardwareRendererObserver observer) {
        throw Unwritten.placeholder(
                "android.view.ThreadedRenderer.removeObserver(android.graphics.HardwareRendererObserver)");
    }
}
