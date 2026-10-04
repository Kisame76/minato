package android.animation;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: PropertyValuesHolder's keyframes, which
 * LayoutTransition compares.
 */
public interface Keyframes extends Cloneable {
    Object getValue(float fraction);
}
