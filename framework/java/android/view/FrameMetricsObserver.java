package android.view;

import android.graphics.HardwareRendererObserver;
import android.os.Handler;
import com.android.minato.services.Unwritten;

/**
 * Placeholder (the Unity test APK retest 11; replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0039 step c2c2): View keeps the observers of
 * addOnFrameMetricsAvailableListener; hwui's GPU pipeline gives frame metrics.
 */
public class FrameMetricsObserver {
    Window.OnFrameMetricsAvailableListener mListener;

    FrameMetricsObserver( Window window, Handler handler, Window.OnFrameMetricsAvailableListener listener) {
        throw Unwritten.placeholder(
                "android.view.FrameMetricsObserver(Window,Handler,Window.OnFrameMetricsAvailableListener)");
    }

    HardwareRendererObserver getRendererObserver() {
        throw Unwritten.placeholder("android.view.FrameMetricsObserver.getRendererObserver()");
    }
}
