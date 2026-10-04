package android.view.translation;

import android.app.Activity;
import android.content.Context;
import android.view.autofill.AutofillId;
import java.io.PrintWriter;
import java.util.List;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Activity's UI translation. */
public class UiTranslationController implements android.util.Dumpable {
    public static final String DUMPABLE_NAME = "UiTranslationController";

    public UiTranslationController(Activity activity, Context context) {
        throw new RuntimeException("stub");
    }

    public void updateUiTranslationState(int state, TranslationSpec sourceSpec, TranslationSpec targetSpec,
            List<AutofillId> views, UiTranslationSpec uiTranslationSpec) {
        throw new RuntimeException("stub");
    }

    public void onActivityDestroyed() {
        throw new RuntimeException("stub");
    }

    public String getDumpableName() {
        throw new RuntimeException("stub");
    }

    public void dump(PrintWriter pw, String[] args) {
        throw new RuntimeException("stub");
    }
}
