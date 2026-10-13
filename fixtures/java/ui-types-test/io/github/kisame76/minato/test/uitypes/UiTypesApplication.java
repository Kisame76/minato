package io.github.kisame76.minato.test.uitypes;

import android.app.Application;
import android.gesture.Gesture;
import android.gesture.GesturePoint;
import android.gesture.GestureStore;
import android.gesture.GestureStroke;
import android.gesture.OrientedBoundingBox;
import android.gesture.Prediction;
import android.graphics.RectF;
import android.graphics.drawable.ColorDrawable;
import android.icu.util.ULocale;
import android.os.Parcel;
import android.sax.Element;
import android.sax.EndElementListener;
import android.sax.EndTextElementListener;
import android.sax.RootElement;
import android.sax.StartElementListener;
import android.speech.AlternativeSpan;
import android.speech.RecognitionSupport;
import android.text.style.DrawableMarginSpan;
import android.text.style.LocaleSpan;
import android.text.style.MaskFilterSpan;
import android.text.style.ScaleXSpan;
import android.text.style.TtsSpan;
import android.util.Log;
import android.util.Xml;
import android.view.textclassifier.ConversationAction;
import android.view.textclassifier.SelectionEvent;
import android.view.textclassifier.TextClassifier;
import android.view.textclassifier.TextLanguage;
import android.view.textclassifier.TextSelection;
import android.webkit.DateSorter;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;
import java.util.TreeMap;
import org.xml.sax.Attributes;

/**
 * The test of framework breadth batch F1 (android.gesture, android.sax, the text spans, the text classifier's data
 * types, DateSorter, the speech result types and the classes that only have to load), used as an app does, without a
 * window. It logs under the tag ui-types-test; tools/android-reference.sh takes the log from Android 15, and
 * tools/ui-types-check.sh compares Minato's with it.
 */
public class UiTypesApplication extends Application {
    private static final String TAG = "ui-types-test";

    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("gesture", UiTypesApplication::gesture);
        part("sax", UiTypesApplication::sax);
        part("spans", UiTypesApplication::spans);
        part("classifier", UiTypesApplication::classifier);
        part("datesorter", this::dateSorter);
        part("speech", UiTypesApplication::speech);
        part("classes", UiTypesApplication::classes);
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            // Its stack differs by device, so it is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    private static String f(float value) {
        return String.format(Locale.ROOT, "%.3f", value);
    }

    private static Gesture line(float x0, float y0, float x1, float y1) {
        Gesture gesture = new Gesture();
        ArrayList<GesturePoint> points = new ArrayList<>();
        for (int i = 0; i <= 8; i++) {
            points.add(new GesturePoint(x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8, 100L * i));
        }
        gesture.addStroke(new GestureStroke(points));
        return gesture;
    }

    private static void gesture() throws Exception {
        Gesture across = line(0, 0, 80, 10);
        RectF box = across.getBoundingBox();
        Log.i(TAG, "gesture: strokes " + across.getStrokesCount() + " length " + f(across.getLength()) + " box "
                + f(box.left) + " " + f(box.top) + " " + f(box.right) + " " + f(box.bottom) + " id "
                + (across.getID() > 0));
        OrientedBoundingBox oriented = across.getStrokes().get(0).computeOrientedBoundingBox();
        Log.i(TAG, "gesture oriented: " + f(oriented.squareness) + " " + f(oriented.orientation) + " "
                + f(oriented.centerX) + " " + f(oriented.centerY) + " " + f(oriented.width) + " " + f(oriented.height));
        GestureStore store = new GestureStore();
        store.addGesture("across", across);
        store.addGesture("down", line(0, 0, 10, 80));
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        store.save(out);
        GestureStore loaded = new GestureStore();
        loaded.load(new ByteArrayInputStream(out.toByteArray()));
        String[] names = loaded.getGestureEntries().toArray(new String[0]);
        Arrays.sort(names);
        Log.i(TAG, "gesture store: " + Arrays.toString(names) + " " + loaded.getGestures("across").size() + " bytes "
                + (out.size() > 0) + " sequence " + loaded.getOrientationStyle() + " " + loaded.getSequenceType());
        for (Prediction p : loaded.recognize(line(0, 0, 70, 12))) {
            Log.i(TAG, "gesture predicts: " + p.name + " " + f((float) p.score));
        }
        loaded.removeGesture("down", loaded.getGestures("down").get(0));
        Log.i(TAG, "gesture removed: " + loaded.getGestureEntries().size() + " " + loaded.hasChanged());
    }

    private static void sax() throws Exception {
        final StringBuilder seen = new StringBuilder();
        RootElement root = new RootElement("feed");
        Element entry = root.getChild("entry");
        entry.setStartElementListener(new StartElementListener() {
            @Override public void start(Attributes attributes) {
                seen.append("[entry ").append(attributes.getValue("id")).append("]");
            }
        });
        entry.getChild("title").setEndTextElementListener(new EndTextElementListener() {
            @Override public void end(String body) {
                seen.append("[title ").append(body).append("]");
            }
        });
        entry.setEndElementListener(new EndElementListener() {
            @Override public void end() {
                seen.append("[/entry]");
            }
        });
        Xml.parse("<feed><entry id=\"1\"><title>One</title></entry><entry id=\"2\"><title>Two</title></entry></feed>",
                root.getContentHandler());
        Log.i(TAG, "sax: " + seen);
    }

    private static Parcel roundTrip(java.util.function.Consumer<Parcel> write) {
        Parcel parcel = Parcel.obtain();
        write.accept(parcel);
        parcel.setDataPosition(0);
        return parcel;
    }

    private static void spans() {
        LocaleSpan locale = new LocaleSpan(Locale.CANADA_FRENCH);
        Parcel p = roundTrip(parcel -> locale.writeToParcel(parcel, 0));
        Log.i(TAG, "spans locale: " + locale.getLocale().toLanguageTag() + " "
                + new LocaleSpan(p).getLocale().toLanguageTag() + " " + locale.getSpanTypeId());
        p.recycle();
        ScaleXSpan scale = new ScaleXSpan(1.5f);
        p = roundTrip(parcel -> scale.writeToParcel(parcel, 0));
        Log.i(TAG, "spans scale: " + f(scale.getScaleX()) + " " + f(new ScaleXSpan(p).getScaleX()) + " "
                + scale.getSpanTypeId());
        p.recycle();
        TtsSpan tts = new TtsSpan.CardinalBuilder(42L).setGender(TtsSpan.GENDER_FEMALE).build();
        TreeMap<String, Object> args = new TreeMap<>();
        for (String key : tts.getArgs().keySet()) {
            args.put(key, tts.getArgs().get(key));
        }
        Log.i(TAG, "spans tts: " + tts.getType() + " " + args);
        Log.i(TAG, "spans tts types: " + TtsSpan.TYPE_TELEPHONE + " " + TtsSpan.TYPE_TIME + " " + TtsSpan.ARG_NUMBER);
        DrawableMarginSpan margin = new DrawableMarginSpan(new ColorDrawable(0xFF00FF00), 6);
        Log.i(TAG, "spans margin: " + margin.getLeadingMargin(true) + " " + margin.getLeadingMargin(false));
        Log.i(TAG, "spans mask: " + (new MaskFilterSpan(null).getMaskFilter() == null));
    }

    private static void classifier() {
        ConversationAction action = new ConversationAction.Builder(ConversationAction.TYPE_CALL_PHONE)
                .setConfidenceScore(0.75f).setTextReply("ok").build();
        Log.i(TAG, "classifier action: " + action.getType() + " " + f(action.getConfidenceScore()) + " "
                + action.getTextReply());
        TextSelection selection = new TextSelection.Builder(2, 9).setEntityType(TextClassifier.TYPE_PHONE, 0.9f)
                .setId("sel").build();
        Log.i(TAG, "classifier selection: " + selection.getSelectionStartIndex() + " "
                + selection.getSelectionEndIndex() + " " + selection.getEntityCount() + " "
                + selection.getEntity(0) + " "
                + f(selection.getConfidenceScore(TextClassifier.TYPE_PHONE)) + " "
                + f(selection.getConfidenceScore(TextClassifier.TYPE_EMAIL)) + " " + selection.getId());
        TextLanguage language = new TextLanguage.Builder().putLocale(ULocale.forLanguageTag("de"), 0.6f).build();
        Log.i(TAG, "classifier language: " + language.getLocaleHypothesisCount() + " "
                + language.getLocale(0).toLanguageTag() + " " + f(language.getConfidenceScore(language.getLocale(0))));
        SelectionEvent event = SelectionEvent.createSelectionStartedEvent(SelectionEvent.INVOCATION_MANUAL, 3);
        Log.i(TAG, "classifier event: " + event.getEventType() + " " + event.getInvocationMethod() + " "
                + event.getStart() + " " + event.getEnd());
        try {
            new TextSelection.Builder(5, 5);
            Log.i(TAG, "classifier empty selection: accepted");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "classifier empty selection: IllegalArgumentException");
        }
    }

    private void dateSorter() {
        DateSorter sorter = new DateSorter(this);
        long now = System.currentTimeMillis();
        Log.i(TAG, "datesorter: " + DateSorter.DAY_COUNT + " " + sorter.getIndex(now) + " "
                + sorter.getIndex(now - 3L * 24 * 3600 * 1000) + " " + sorter.getIndex(now - 40L * 24 * 3600 * 1000));
        Log.i(TAG, "datesorter label: " + sorter.getLabel(0) + " | " + sorter.getLabel(DateSorter.DAY_COUNT - 1));
    }

    private static void speech() {
        RecognitionSupport support = new RecognitionSupport.Builder()
                .addInstalledOnDeviceLanguage("en-US").addPendingOnDeviceLanguage("de-DE")
                .addSupportedOnDeviceLanguage("fr-FR").addOnlineLanguage("es-ES").build();
        Log.i(TAG, "speech support: " + support.getInstalledOnDeviceLanguages() + " "
                + support.getPendingOnDeviceLanguages() + " " + support.getSupportedOnDeviceLanguages() + " "
                + support.getOnlineLanguages());
        AlternativeSpan span = new AlternativeSpan(0, 5, Arrays.asList("alpha", "beta"));
        Log.i(TAG, "speech span: " + span.getStartPosition() + " " + span.getEndPosition() + " "
                + span.getAlternatives());
    }

    private static void classes() {
        String[] names = {"android.gesture.GestureLibraries", "android.gesture.GestureLibrary",
            "android.gesture.GestureOverlayView", "android.gesture.GestureUtils", "android.sax.ElementListener",
            "android.sax.TextElementListener", "android.print.PrintJobId", "android.print.PrinterCapabilitiesInfo",
            "android.print.PrinterId", "android.speech.AlternativeSpans", "android.speech.ModelDownloadListener",
            "android.speech.RecognitionListener", "android.speech.RecognitionPart",
            "android.speech.RecognitionSupportCallback", "android.text.style.IconMarginSpan",
            "android.view.textclassifier.ConversationActions",
            "android.view.textclassifier.TextClassificationSessionId",
            "android.view.textclassifier.TextClassifierEvent", "android.webkit.PluginStub",
            "android.webkit.ServiceWorkerController", "android.webkit.TracingController",
            "android.webkit.WebIconDatabase", "android.webkit.WebViewDatabase", "android.webkit.WebViewFragment"};
        for (String name : names) {
            try {
                Class<?> c = Class.forName(name, false, UiTypesApplication.class.getClassLoader());
                Log.i(TAG, "class " + name + ": loads " + c.isInterface());
            } catch (Throwable t) {
                Log.i(TAG, "class " + name + ": " + t.getClass().getName());
            }
        }
    }
}
