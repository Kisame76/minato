package com.android.input.flags;

/**
 * Compile-only stub (ADR-0045, the input service), never in framework.jar: the input flags' names that InputManager and
 * InputDevice give @FlaggedApi. Their declarations are frameworks/native's, which is not fetched; javac inlines the
 * names.
 */
public final class Flags {
    public static final String FLAG_INPUT_DEVICE_VIEW_BEHAVIOR_API =
            "com.android.input.flags.input_device_view_behavior_api";

    public static final String FLAG_DEVICE_ASSOCIATIONS = "com.android.input.flags.device_associations";
}
