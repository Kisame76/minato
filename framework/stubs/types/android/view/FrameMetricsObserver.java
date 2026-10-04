package android.view;

import android.graphics.HardwareRendererObserver;
import android.os.Handler;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View keeps the observers of
 * addOnFrameMetricsAvailableListener; hwui's GPU pipeline gives frame metrics.
 */
public class FrameMetricsObserver {
    Window.OnFrameMetricsAvailableListener mListener;

    FrameMetricsObserver( Window window, Handler handler, Window.OnFrameMetricsAvailableListener listener) {
        throw new RuntimeException("stub");
    }

    HardwareRendererObserver getRendererObserver() {
        throw new RuntimeException("stub");
    }
}
