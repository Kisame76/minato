package com.android.internal.view;

import android.content.Context;
import android.view.View;

/** Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View shows tooltips with it. */
public class TooltipPopup {
    public TooltipPopup(Context context) {
        throw new RuntimeException("stub");
    }

    public void show(View anchorView, int anchorX, int anchorY, boolean fromTouch, CharSequence tooltipText) {
        throw new RuntimeException("stub");
    }

    public void hide() {
        throw new RuntimeException("stub");
    }

    public View getContentView() {
        throw new RuntimeException("stub");
    }
}
