package io.github.kisame76.minato.test.imehide;

import android.inputmethodservice.InputMethodService;
import android.view.View;
import android.view.ViewGroup;

/** ADR-0068 step 2d's fixture input method: a keyboard of a fixed height that types nothing. */
public class FixtureInputMethod extends InputMethodService {
    static final int KEYBOARD_HEIGHT_PX = 600;

    @Override
    public View onCreateInputView() {
        View keyboard = new View(this);
        keyboard.setLayoutParams(new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, KEYBOARD_HEIGHT_PX));
        keyboard.setMinimumHeight(KEYBOARD_HEIGHT_PX);
        return keyboard;
    }
}
