package io.github.kisame76.minato.test.links;

import android.app.Application;
import android.app.AppOpsManager;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.text.Spannable;
import android.text.SpannableString;
import android.text.method.LinkMovementMethod;
import android.text.style.URLSpan;
import android.text.util.Linkify;
import android.util.Log;
import android.widget.TextView;

import java.util.concurrent.Callable;
import java.util.regex.Pattern;

/**
 * Linkify and AppOpsManager.checkPackage (the compatibility plan's decision 6): the links Linkify finds in a text, in
 * the order of the text, and the package check of the app ops for the app's own uid and for a package that is not
 * its. Phone numbers need libphonenumber, which Minato lacks, so what Linkify finds for them is tagged "-device" and
 * only shown.
 */
public class LinksApplication extends Application {
    private static final String TAG = "links-answers-test";

    private static void log(String name, Callable<Object> call) {
        String result;
        try {
            result = String.valueOf(call.call());
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(TAG, name + ": " + result);
    }

    private static String spans(Spannable text) {
        final StringBuilder out = new StringBuilder();
        for (URLSpan span : text.getSpans(0, text.length(), URLSpan.class)) {
            out.append(span.getURL()).append('@').append(text.getSpanStart(span)).append('-')
                    .append(text.getSpanEnd(span)).append(' ');
        }
        return out.toString().trim();
    }

    private static Spannable linked(String text, int mask) {
        final SpannableString spannable = new SpannableString(text);
        Linkify.addLinks(spannable, mask);
        return spannable;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        final String text = "see http://example.com/a and www.example.org or mail me@example.com now";
        log("web links", () -> spans(linked(text, Linkify.WEB_URLS)));
        log("e-mail links", () -> spans(linked(text, Linkify.EMAIL_ADDRESSES)));
        log("web and e-mail links", () -> spans(linked(text, Linkify.WEB_URLS | Linkify.EMAIL_ADDRESSES)));
        log("addLinks result with a link", () -> Linkify.addLinks(new SpannableString(text), Linkify.WEB_URLS));
        log("addLinks result without a link",
                () -> Linkify.addLinks(new SpannableString("nothing here"), Linkify.WEB_URLS));
        log("addLinks result with the mask 0", () -> Linkify.addLinks(new SpannableString(text), 0));
        log("pattern links", () -> {
            final SpannableString spannable = new SpannableString("order 123 and 45");
            Linkify.addLinks(spannable, Pattern.compile("\\d+"), "num:");
            return spans(spannable);
        });
        log("text view links", () -> {
            final TextView view = new TextView(this);
            view.setText(text);
            final boolean result = Linkify.addLinks(view, Linkify.WEB_URLS);
            return result + " " + spans((Spannable) view.getText()) + " "
                    + (view.getMovementMethod() instanceof LinkMovementMethod);
        });
        log("existing links are replaced", () -> {
            final SpannableString spannable = new SpannableString("go to http://example.com now");
            Linkify.addLinks(spannable, Linkify.WEB_URLS);
            Linkify.addLinks(spannable, Linkify.WEB_URLS);
            return spans(spannable);
        });
        Log.i(TAG + "-device", "phone links: " + spans(linked("call +49 30 1234567 now", Linkify.PHONE_NUMBERS)));

        final AppOpsManager appOps = getSystemService(AppOpsManager.class);
        log("checkPackage of the app's own", () -> {
            appOps.checkPackage(Process.myUid(), getPackageName());
            return "returned";
        });
        log("checkPackage of another package", () -> {
            appOps.checkPackage(Process.myUid(), "com.android.settings");
            return "returned";
        });
        Log.i(TAG, "done");
    }
}
