package android.view.textclassifier;

import android.app.RemoteAction;
import android.os.Bundle;

/** Compile-only stub (ADR-0043 step d)), never in framework.jar: a text classification's extras, for a selection's toolbar. */
public final class ExtrasUtils {
    public static Bundle getForeignLanguageExtra(TextClassification classification) {
        throw new RuntimeException("stub");
    }

    public static RemoteAction findTranslateAction(TextClassification classification) {
        throw new RuntimeException("stub");
    }

    public static String getEntityType(Bundle extra) {
        throw new RuntimeException("stub");
    }

    public static float getScore(Bundle extra) {
        throw new RuntimeException("stub");
    }

    public static String getModelName(Bundle extra) {
        throw new RuntimeException("stub");
    }
}
