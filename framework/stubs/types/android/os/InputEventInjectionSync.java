package android.os;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: frameworks/native's AIDL enum of injection
 * modes, which InputManager's injectInputEvent names. javac inlines the values, InputEventInjectionSync.aidl's at the
 * tag.
 */
public @interface InputEventInjectionSync {
    int NONE = 0;

    int WAIT_FOR_RESULT = 1;

    int WAIT_FOR_FINISHED = 2;
}
