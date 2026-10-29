package android.content.pm;

import android.app.appsearch.GenericDocument;

/**
 * Compile-only stub (gap 1, batch 3), never in framework.jar: a shortcut as AppSearch stores it, which
 * ShortcutInfo.createFromGenericDocument converts. Minato has no AppSearch.
 */
public class AppSearchShortcutInfo {
    public AppSearchShortcutInfo(GenericDocument document) {
        throw new RuntimeException("stub");
    }

    public ShortcutInfo toShortcutInfo(int userId) {
        throw new RuntimeException("stub");
    }
}
