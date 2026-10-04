package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: a keyboard layout's preview, which
 * InputManager draws for the Settings app.
 */
final class PhysicalKeyLayout {
    PhysicalKeyLayout(android.view.KeyCharacterMap keyCharacterMap, KeyboardLayout layout) {
        throw new RuntimeException("stub");
    }
}
