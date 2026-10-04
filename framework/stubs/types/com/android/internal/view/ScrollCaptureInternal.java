package com.android.internal.view;

import android.graphics.Point;
import android.graphics.Rect;
import android.view.ScrollCaptureCallback;
import android.view.View;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: AttachInfo's scroll capture helper; scroll capture is
 * a system screenshot feature.
 */
public class ScrollCaptureInternal {
    public ScrollCaptureCallback requestCallback(View view, Rect localVisibleRect, Point positionInWindow) {
        throw new RuntimeException("stub");
    }
}
