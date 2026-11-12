package com.android.internal.minato.webkit;

import android.view.View;
import android.view.ViewParent;

import java.util.ArrayList;
import java.util.List;

/**
 * The WebViews whose page shows through a hole in their window: attached, with the host's view open. View's draw asks
 * whether a view with a hardware layer has one of them below it (the patch view-hardware-layer-over-a-webview), since
 * such a layer, drawn in software through a drawing cache, would keep the hole in the cache's bitmap. On the main
 * thread only, as drawing is.
 */
public final class WebViewHoles {
    private static final List<View> sViews = new ArrayList<>();

    private WebViewHoles() {}

    static void add(View webView) {
        if (!sViews.contains(webView)) {
            sViews.add(webView);
        }
    }

    static void remove(View webView) {
        sViews.remove(webView);
    }

    /** Whether one of the WebViews lies below `view` (is `view` or one of its descendants). */
    public static boolean above(View view) {
        for (int i = 0; i < sViews.size(); i++) {
            View webView = sViews.get(i);
            if (webView == view) {
                return true;
            }
            for (ViewParent parent = webView.getParent(); parent != null; parent = parent.getParent()) {
                if (parent == view) {
                    return true;
                }
            }
        }
        return false;
    }
}
