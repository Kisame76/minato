package com.android.internal.view;

import android.content.Context;
import android.view.View;
import com.android.minato.services.Unwritten;

/**
 * Placeholder (App, the Unity test APK retest 11; Grafik owns it, replaced by ADR-0058 b) / category 2): our class under AOSP's
 * hidden name, which only has to load, since a class apps reflect over names it; every body logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (ADR-0039 step c2c2): View shows tooltips with it.
 */
public class TooltipPopup {
    public TooltipPopup(Context context) {
        throw Unwritten.placeholder("com.android.internal.view.TooltipPopup(Context)");
    }

    public void show(View anchorView, int anchorX, int anchorY, boolean fromTouch, CharSequence tooltipText) {
        throw Unwritten.placeholder("com.android.internal.view.TooltipPopup.show(View,int,int,boolean,CharSequence)");
    }

    public void hide() {
        throw Unwritten.placeholder("com.android.internal.view.TooltipPopup.hide()");
    }

    public View getContentView() {
        throw Unwritten.placeholder("com.android.internal.view.TooltipPopup.getContentView()");
    }
}
