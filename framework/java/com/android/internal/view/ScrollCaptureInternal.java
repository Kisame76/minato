package com.android.internal.view;

import android.graphics.Point;
import android.graphics.Rect;
import android.view.ScrollCaptureCallback;
import android.view.View;
import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the Unity test APK retest 11; Grafik owns it, replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0039 step c2c2): AttachInfo's scroll capture helper;
 * scroll capture is a system screenshot feature.
 */
public class ScrollCaptureInternal {
    public ScrollCaptureInternal() {
        throw Unwritten.placeholder("com.android.internal.view.ScrollCaptureInternal()");
    }

    public ScrollCaptureCallback requestCallback(View view, Rect localVisibleRect, Point positionInWindow) {
        throw Unwritten.placeholder("com.android.internal.view.ScrollCaptureInternal.requestCallback(View,Rect,Point)");
    }
}
