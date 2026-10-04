package android.view.autofill;

import android.graphics.Rect;
import android.view.View;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: AutofillClientController's popup, for autofill. */
public class AutofillPopupWindow {
    public AutofillPopupWindow(IAutofillWindowPresenter presenter) { throw new RuntimeException("stub"); }
    public boolean isShowing() { throw new RuntimeException("stub"); }
    public void dismiss() { throw new RuntimeException("stub"); }
    public void update(View anchor, int offsetX, int offsetY, int width, int height,
            Rect virtualBounds) { throw new RuntimeException("stub"); }
}
