package android.security;

/**
 * Compile-only stub (batch D 2), never in framework.jar: the constant KeyGenParameterSpec's Builder inlines as its
 * default secure user ID. GateKeeper's service is the system's; no app reaches it.
 */
public abstract class GateKeeper {
    public static final long INVALID_SECURE_USER_ID = 0;

    private GateKeeper() {
    }
}
