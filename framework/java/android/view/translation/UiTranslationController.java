/*
 * Activity's UI translation (the Unity test APK retest 10): our class under AOSP's hidden name, since Activity's methods and fields
 * name it and a class whose member types cannot be resolved cannot be read by reflection. AOSP's controller translates
 * an activity's views for the system's translation service, which Minato does not have (no Context gives a
 * UiTranslationManager), so no state update ever comes and the controller keeps nothing.
 */
package android.view.translation;

import android.app.Activity;
import android.content.Context;
import android.util.Dumpable;
import android.view.autofill.AutofillId;
import java.io.PrintWriter;
import java.util.List;

/** @hide */
public class UiTranslationController implements Dumpable {
    public static final String DUMPABLE_NAME = "UiTranslationController";

    /** TextView's (ADR-0043 step d)): its translation callback logs only with it. */
    public static final boolean DEBUG = false;

    public UiTranslationController(Activity activity, Context context) {
    }

    /** The system's request to translate the views; none comes on Minato. */
    public void updateUiTranslationState(int state, TranslationSpec sourceSpec, TranslationSpec targetSpec,
            List<AutofillId> views, UiTranslationSpec uiTranslationSpec) {
    }

    public void onActivityDestroyed() {
    }

    @Override
    public String getDumpableName() {
        return DUMPABLE_NAME;
    }

    @Override
    public void dump(PrintWriter pw, String[] args) {
    }
}
