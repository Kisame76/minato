package android.view;

import android.graphics.Canvas;
import android.graphics.Rect;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View draws round scroll bars with it on round (watch)
 * screens.
 */
public class RoundScrollbarRenderer {
    public RoundScrollbarRenderer(View parent) {
        throw new RuntimeException("stub");
    }

    public void drawRoundScrollbars(Canvas canvas, float alpha, Rect bounds, boolean drawToLeft) {
        throw new RuntimeException("stub");
    }

    void getRoundVerticalScrollBarBounds(Rect bounds) {
        throw new RuntimeException("stub");
    }
}
