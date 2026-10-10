package android.graphics;

/**
 * Compile-only stub (ADR-0058 a)), never in framework.jar: HardwareBuffer.createFromGraphicBuffer takes libui's buffer,
 * a hidden type that Minato's HardwareBuffer has no counterpart of; its natives answer 0 for it.
 */
public class GraphicBuffer {
}
