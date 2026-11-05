package io.github.kisame76.minato.test.textclassifier;

import android.app.Application;
import android.os.Handler;
import android.os.LocaleList;
import android.os.Looper;
import android.util.Log;
import android.view.textclassifier.TextClassification;
import android.view.textclassifier.TextClassificationManager;
import android.view.textclassifier.TextClassifier;
import android.view.textclassifier.TextLinks;
import android.view.textclassifier.TextSelection;

import java.util.concurrent.Callable;

/**
 * TextClassificationManager (the compatibility plan's decision 6): the manager and its classifier, and what the
 * classifier answers for a URL, a phone number, links and a selection. What differs by device (the class of the
 * classifier) is tagged "-device" and only shown.
 */
public class ClassifierApplication extends Application {
    private static final String TAG = "text-classifier-test";

    private static void log(String name, Callable<Object> call) {
        String result;
        try {
            result = String.valueOf(call.call());
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(TAG, name + ": " + result);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        final TextClassificationManager manager = getSystemService(TextClassificationManager.class);
        log("manager is not null", () -> manager != null);
        log("text classifier is not null", () -> manager.getTextClassifier() != null);
        log("text classifier is the same object twice",
                () -> manager.getTextClassifier() == manager.getTextClassifier());
        final TextClassifier classifier = manager.getTextClassifier();
        Log.i(TAG + "-device", "text classifier class: " + classifier.getClass().getName());
        log("classifyText of a URL", () -> entities(classifier.classifyText("see http://example.com/a now", 4, 24,
                LocaleList.getDefault())));
        log("classifyText of a phone number", () -> entities(classifier.classifyText("call +49 30 1234567 now", 5, 19,
                LocaleList.getDefault())));
        log("generateLinks count", () -> classifier.generateLinks(new TextLinks.Request.Builder(
                "mail me@example.com or visit http://example.com today").build()).getLinks().size());
        log("suggested selection of a word inside a URL", () -> {
            final TextSelection selection = classifier.suggestSelection(
                    new TextSelection.Request.Builder("see http://example.com/a now", 8, 9).build());
            return selection.getSelectionStartIndex() + " " + selection.getSelectionEndIndex();
        });
        log("max length of the text for links", () -> classifier.getMaxGenerateLinksTextLength());
        log("classifier is not destroyed", () -> !classifier.isDestroyed());
        log("an app's own classifier comes back", () -> {
            final TextClassifier own = new TextClassifier() { };
            manager.setTextClassifier(own);
            final boolean same = manager.getTextClassifier() == own;
            manager.setTextClassifier(null);
            return same;
        });
        Log.i(TAG, "done");
    }

    private static String entities(TextClassification result) {
        return result.getEntityCount() + " " + (result.getEntityCount() > 0 ? result.getEntity(0) : "-");
    }
}
