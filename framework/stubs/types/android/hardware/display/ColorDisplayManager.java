package android.hardware.display;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 1), never in framework.jar: the color display service's manager, which
 * DisplayManager.setSaturationLevel asks, a system API that needs CONTROL_DISPLAY_SATURATION, which no app holds.
 */
public final class ColorDisplayManager {
    public boolean setSaturationLevel(int saturationLevel) {
        throw new RuntimeException("stub");
    }
}
