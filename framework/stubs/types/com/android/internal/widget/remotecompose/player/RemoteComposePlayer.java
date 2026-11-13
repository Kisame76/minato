package com.android.internal.widget.remotecompose.player;

import android.content.Context;

/**
 * Compile-only stub (ADR-0045), never in framework.jar: the player of RemoteViews' draw instructions (Android 15's
 * RemoteCompose), which only a host that applies
 * RemoteViews plays; Minato hosts none.
 */
public class RemoteComposePlayer extends android.widget.FrameLayout {
    public RemoteComposePlayer(Context context) {
        super(context);
        throw new RuntimeException("stub");
    }

    public void setDocument(RemoteComposeDocument value) {
        throw new RuntimeException("stub");
    }

    public void setDebug(int debugFlags) {
        throw new RuntimeException("stub");
    }

    public void setTheme(int theme) {
        throw new RuntimeException("stub");
    }

    public void addClickListener(ClickCallbacks callback) {
        throw new RuntimeException("stub");
    }

    public interface ClickCallbacks {
        void click(int id, String metadata);
    }
}
