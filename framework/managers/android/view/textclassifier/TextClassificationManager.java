// The written part of TextClassificationManager (ADR-0043 step d), decision 3), which tools/managergen joins with the
// API's methods: the class TextView names and its selection helper asks for the settings, at every text change. Text
// classification is absent, so getSystemService answers null and the settings are the defaults, as AOSP's falls back
// when there is no manager (TextClassificationManager.java:218-228 at android-15.0.0_r1).
import android.content.Context;

import java.util.Objects;

class TextClassificationManager {
    private static final TextClassificationConstants sDefaultSettings = new TextClassificationConstants();

    /** @hide As AOSP's: the manager's settings, or the defaults where there is none. */
    public static TextClassificationConstants getSettings(Context context) {
        Objects.requireNonNull(context);
        final TextClassificationManager tcm = context.getSystemService(TextClassificationManager.class);
        return tcm != null ? tcm.getSettings() : sDefaultSettings;
    }

    /** @hide */
    public TextClassificationConstants getSettings() {
        return sDefaultSettings;
    }

    /** @hide TextView's, for a selection or links; no manager is made, so none is asked. */
    public TextClassifier createTextClassificationSession(TextClassificationContext classificationContext,
            TextClassifier textClassifier) {
        return textClassifier;
    }
}
