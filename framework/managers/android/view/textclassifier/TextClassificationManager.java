// The written part of TextClassificationManager (ADR-0043 step d), decision 3), which tools/managergen joins with the
// API's methods: the class TextView names and its selection helper asks for the settings, at every text change. The
// manager is there, as on Android, and its classifier is one that finds nothing: no entities, no links and a
// selection that stays as it is, as the emulator's (apk-runner-35) answers when it has no text classifier model
// [verified: text-classifier-test]; a new object for every call, as Android gives one. An app's own classifier, set
// with setTextClassifier, is what it gets back.
import android.content.Context;

import java.util.Objects;

class TextClassificationManager {
    private static final TextClassificationConstants sDefaultSettings = new TextClassificationConstants();

    private final Context mContext;
    private TextClassifier mCustomTextClassifier;
    private TextClassificationSessionFactory mSessionFactory;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public TextClassificationManager(Context context) {
        mContext = Objects.requireNonNull(context);
    }

    public TextClassifier getTextClassifier() {
        synchronized (this) {
            if (mCustomTextClassifier != null) {
                return mCustomTextClassifier;
            }
        }
        return new TextClassifier() {
            /** The on-device classifier's limit, which Android answers [verified: text-classifier-test]. */
            @Override
            public int getMaxGenerateLinksTextLength() {
                return 100_000;
            }
        };
    }

    public void setTextClassifier(TextClassifier textClassifier) {
        synchronized (this) {
            mCustomTextClassifier = textClassifier;
        }
    }

    public void setTextClassificationSessionFactory(TextClassificationSessionFactory factory) {
        synchronized (this) {
            mSessionFactory = factory;
        }
    }

    public TextClassifier createTextClassificationSession(TextClassificationContext classificationContext) {
        synchronized (this) {
            if (mSessionFactory != null) {
                return mSessionFactory.createTextClassificationSession(classificationContext);
            }
        }
        return getTextClassifier();
    }

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
