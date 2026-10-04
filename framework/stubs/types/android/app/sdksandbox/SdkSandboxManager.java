package android.app.sdksandbox;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: the SDK runtime module's (not fetched); Intent
 * names its sandboxed activity action, which javac inlines with AOSP's value.
 */
public final class SdkSandboxManager {
    public static final String ACTION_START_SANDBOXED_ACTIVITY = "android.app.sdksandbox.action.START_SANDBOXED_ACTIVITY";
}
