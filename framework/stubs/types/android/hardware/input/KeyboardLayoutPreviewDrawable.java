package android.hardware.input;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: a keyboard layout's preview, which
 * InputManager draws for the Settings app.
 */
final class KeyboardLayoutPreviewDrawable extends android.graphics.drawable.Drawable {
    KeyboardLayoutPreviewDrawable(android.content.Context context, PhysicalKeyLayout layout, int width, int height) {
        throw new RuntimeException("stub");
    }

    public void draw(android.graphics.Canvas canvas) {
        throw new RuntimeException("stub");
    }

    public void setAlpha(int alpha) {
        throw new RuntimeException("stub");
    }

    public void setColorFilter(android.graphics.ColorFilter colorFilter) {
        throw new RuntimeException("stub");
    }

    public int getOpacity() {
        throw new RuntimeException("stub");
    }
}
