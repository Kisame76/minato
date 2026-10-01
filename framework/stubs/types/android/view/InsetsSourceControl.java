package android.view;

import android.graphics.Insets;

/** Compile-only stub (ADR-0039 b3.1), never in framework.jar: CompatibilityInfo.Translator scales its hint. */
public class InsetsSourceControl {
    public Insets getInsetsHint() {
        throw new RuntimeException("stub");
    }

    public void setInsetsHint(Insets hint) {
        throw new RuntimeException("stub");
    }

    public void setInsetsHint(int left, int top, int right, int bottom) {
        throw new RuntimeException("stub");
    }
}
