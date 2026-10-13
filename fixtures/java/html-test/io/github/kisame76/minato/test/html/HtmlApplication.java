package io.github.kisame76.minato.test.html;

import android.app.Application;
import android.text.Html;
import android.text.Spanned;
import android.text.style.AbsoluteSizeSpan;
import android.text.style.AlignmentSpan;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.text.style.ImageSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.StyleSpan;
import android.text.style.TypefaceSpan;
import android.text.style.URLSpan;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The test of android.text.Html, without a window (framework breadth, step (a) after batch B4). It logs under the tag
 * html-test, for tags that SDK dialogs and notices use, the text that fromHtml makes (line breaks shown as "\n") and
 * its spans (class, range and the property that matters), then toHtml of the result and escapeHtml. Public API only.
 * tools/android-reference.sh takes the log from Android 15, and tools/html-check.sh compares Minato's with it.
 */
public class HtmlApplication extends Application {
    private static final String TAG = "html-test";

    private static final String[] SOURCES = {
        "<b>bold</b> and <i>it</i> and <b><i>both</i></b>",
        "<a href=\"http://example.com/z?x=1&amp;y=2\">link</a> text",
        "<font color=\"#ff0000\">red</font> <font color=\"blue\">blue</font> <font face=\"sans-serif\">sans</font>",
        "<p>para1</p><p>para2</p>after",
        "line<br>break<br/>again",
        "<ul><li>one</li><li>two</li></ul>tail",
        "&lt;tag&gt; &amp; &copy; &#65; &#x42; &nbsp;|",
        "<h1>Head</h1>text<h3>Sub</h3>",
        "<blockquote>quoted</blockquote>plain",
        "<u>u</u><strike>s</strike><sup>2</sup><sub>3</sub><tt>mono</tt><big>b</big><small>s</small>",
        "<span style=\"color:#00ff00;background-color:#0000ff\">styled</span>",
        "<div align=\"center\">centered</div><div align=\"right\">right</div>",
        "<img src=\"x.png\"> pic",
        "<b>unclosed <i>and nested",
        "a<<>>b",
        "  spaced   \n  text  ",
        "",
    };

    @Override
    public void onCreate() {
        super.onCreate();
        for (int mode : new int[] {Html.FROM_HTML_MODE_LEGACY, Html.FROM_HTML_MODE_COMPACT}) {
            for (String source : SOURCES) {
                try {
                    Spanned spanned = Html.fromHtml(source, mode, null, null);
                    Log.i(TAG, "mode " + mode + " '" + source.replace("\n", "\\n") + "': " + dump(spanned));
                } catch (Throwable t) {
                    Log.i(TAG, "mode " + mode + " '" + source.replace("\n", "\\n") + "': failed: " + t);
                    Log.i(TAG + "-device", Log.getStackTraceString(t));
                }
            }
        }
        part("tohtml", () -> {
            for (String source : SOURCES) {
                Spanned spanned = Html.fromHtml(source, Html.FROM_HTML_MODE_LEGACY);
                Log.i(TAG, "toHtml '" + source.replace("\n", "\\n") + "': "
                        + Html.toHtml(spanned, Html.TO_HTML_PARAGRAPH_LINES_CONSECUTIVE).replace("\n", "\\n") + " | "
                        + Html.toHtml(spanned, Html.TO_HTML_PARAGRAPH_LINES_INDIVIDUAL).replace("\n", "\\n"));
            }
        });
        part("escape", () -> Log.i(TAG, "escapeHtml: " + Html.escapeHtml("a<b>&\"'  c\n\td é 😀")));
        Log.i(TAG, "done");
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    /** The text, then each span as class(property)[start,end], ordered by start, end and description. */
    private static String dump(Spanned spanned) {
        List<String> spans = new ArrayList<>();
        for (Object span : spanned.getSpans(0, spanned.length(), Object.class)) {
            spans.add(String.format("%08d,%08d,%s", spanned.getSpanStart(span), spanned.getSpanEnd(span), describe(span)));
        }
        Collections.sort(spans);
        StringBuilder out = new StringBuilder("'").append(spanned.toString().replace("\n", "\\n")).append("' ");
        for (String span : spans) {
            String[] parts = span.split(",", 3);
            out.append(parts[2]).append('[').append(Integer.parseInt(parts[0])).append(',')
                    .append(Integer.parseInt(parts[1])).append("] ");
        }
        return out.toString().trim();
    }

    private static String describe(Object span) {
        String name = span.getClass().getSimpleName();
        if (span instanceof StyleSpan) {
            return name + "(" + ((StyleSpan) span).getStyle() + ")";
        } else if (span instanceof URLSpan) {
            return name + "(" + ((URLSpan) span).getURL() + ")";
        } else if (span instanceof ForegroundColorSpan) {
            return name + "(" + Integer.toHexString(((ForegroundColorSpan) span).getForegroundColor()) + ")";
        } else if (span instanceof BackgroundColorSpan) {
            return name + "(" + Integer.toHexString(((BackgroundColorSpan) span).getBackgroundColor()) + ")";
        } else if (span instanceof RelativeSizeSpan) {
            return name + "(" + ((RelativeSizeSpan) span).getSizeChange() + ")";
        } else if (span instanceof AbsoluteSizeSpan) {
            return name + "(" + ((AbsoluteSizeSpan) span).getSize() + ")";
        } else if (span instanceof TypefaceSpan) {
            return name + "(" + ((TypefaceSpan) span).getFamily() + ")";
        } else if (span instanceof AlignmentSpan) {
            return name + "(" + ((AlignmentSpan) span).getAlignment() + ")";
        } else if (span instanceof ImageSpan) {
            return name + "(" + ((ImageSpan) span).getSource() + ")";
        }
        return name;
    }
}
