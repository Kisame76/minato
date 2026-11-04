package com.android.minato.view;

/**
 * Whether the bar above the keyboard shows a field's text as bullets (ADR-0068, keyboard plan b): for the input types
 * whose text Android's keyboards hide, a text, web or number password. A visible password is not one: Android shows
 * its text, without suggestions, and SDL2's DummyEdit asks for it only to turn suggestions off.
 */
public final class TextMask {
    private static final int MASK_CLASS = 0xf, MASK_VARIATION = 0xff0;
    private static final int CLASS_TEXT = 0x1, CLASS_NUMBER = 0x2;
    private static final int TEXT_PASSWORD = 0x80, TEXT_WEB_PASSWORD = 0xe0, NUMBER_PASSWORD = 0x10;

    private TextMask() {
    }

    /** Whether a field of `inputType` (EditorInfo's) shows bullets for its text. */
    public static boolean masks(int inputType) {
        final int variation = inputType & MASK_VARIATION;
        switch (inputType & MASK_CLASS) {
            case CLASS_TEXT:
                return variation == TEXT_PASSWORD || variation == TEXT_WEB_PASSWORD;
            case CLASS_NUMBER:
                return variation == NUMBER_PASSWORD;
            default:
                return false;
        }
    }
}
