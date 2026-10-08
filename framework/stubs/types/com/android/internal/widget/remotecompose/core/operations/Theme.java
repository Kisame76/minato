package com.android.internal.widget.remotecompose.core.operations;

/**
 * Compile-only stub (ADR-0045 gap 1, batch 2), never in framework.jar: the theme operation of RemoteViews' draw instructions (Android 15's RemoteCompose), which only a host that applies
 * RemoteViews plays; Minato hosts none; javac
 * inlines its constants, with their values at android-15.0.0_r1.
 */
public class Theme {
    public static final int DARK = -2;
    public static final int LIGHT = -3;
}
