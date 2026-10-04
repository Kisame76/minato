package android.util;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View throws it when onDetachedFromWindow and the like
 * skip super.
 */
public final class SuperNotCalledException extends android.util.AndroidRuntimeException {
    public SuperNotCalledException(String msg) {
        super(msg);
    }
}
