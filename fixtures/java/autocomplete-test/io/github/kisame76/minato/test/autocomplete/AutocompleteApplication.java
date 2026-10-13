package io.github.kisame76.minato.test.autocomplete;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.text.util.Rfc822Token;
import android.text.util.Rfc822Tokenizer;
import android.util.Log;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.AutoCompleteTextView;
import android.widget.Filter;
import android.widget.ListPopupWindow;
import android.widget.MultiAutoCompleteTextView;
import android.widget.PopupWindow;

/**
 * The test of the text completion widgets, without a window (framework breadth, step (f)). It logs under the tag
 * autocomplete-test what an app sees of AutoCompleteTextView (threshold, adapter, filtering through its Filter and the
 * selection's text), MultiAutoCompleteTextView with the comma and the RFC 822 tokenizer, Rfc822Tokenizer itself, and
 * ListPopupWindow and PopupWindow as an app configures them (no window is shown, which Minato does not do for a popup yet). The filter runs on its own thread
 * and answers on the main thread, so the filter steps chain from the callbacks. Public API only.
 * tools/android-reference.sh takes the log from Android 15, and tools/autocomplete-check.sh compares Minato's with it.
 */
public class AutocompleteApplication extends Application {
    private static final String TAG = "autocomplete-test";

    private interface Body {
        void run() throws Exception;
    }

    private static void probe(String name, Body body) {
        try {
            body.run();
        } catch (Throwable t) {
            Log.i(TAG, name + " failed: " + t.getClass().getName());
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        probe("rfc822", this::rfc822);
        probe("tokenizers", this::tokenizers);
        probe("popupWindow", this::popupWindow);
        probe("listPopup", this::listPopup);
        probe("autoComplete", this::autoComplete);
    }

    private void rfc822() {
        Rfc822Token[] tokens = Rfc822Tokenizer.tokenize("Ann Lee <ann@example.com>, bob@example.org, \"Cy, D\" <cy@x.y> (work)");
        Log.i(TAG, "rfc822 " + tokens.length);
        for (Rfc822Token token : tokens) {
            Log.i(TAG, "token name=" + token.getName() + " address=" + token.getAddress() + " comment="
                    + token.getComment() + " string=" + token);
        }
        Log.i(TAG, "quote " + Rfc822Token.quoteName("A \"B\" C") + " " + Rfc822Token.quoteComment("c(d)e") + " "
                + Rfc822Token.quoteNameIfNecessary("Doe, John") + " " + Rfc822Token.quoteNameIfNecessary("John"));
        Rfc822Token made = new Rfc822Token("N", "a@b.c", "x");
        Log.i(TAG, "made " + made + " " + made.getName() + " " + made.getAddress());
        Log.i(TAG, "empty " + Rfc822Tokenizer.tokenize("").length + " " + Rfc822Tokenizer.tokenize("   ").length);
    }

    private void tokenizers() {
        MultiAutoCompleteTextView.CommaTokenizer comma = new MultiAutoCompleteTextView.CommaTokenizer();
        String text = "one, two,three";
        Log.i(TAG, "comma " + comma.findTokenStart(text, 7) + " " + comma.findTokenEnd(text, 7) + " "
                + comma.findTokenStart(text, 13) + " " + comma.findTokenEnd(text, 4));
        Log.i(TAG, "comma terminate [" + comma.terminateToken("two") + "] [" + comma.terminateToken("two ") + "] ["
                + comma.terminateToken("two,") + "]");
        Rfc822Tokenizer rfc = new Rfc822Tokenizer();
        String addresses = "ann@example.com, Bob <bob@example.org>";
        Log.i(TAG, "rfc " + rfc.findTokenStart(addresses, 30) + " " + rfc.findTokenEnd(addresses, 3) + " "
                + rfc.findTokenEnd(addresses, 30));
        Log.i(TAG, "rfc terminate [" + rfc.terminateToken("a@b.c") + "]");
    }

    private void popupWindow() {
        View content = new View(this);
        PopupWindow popup = new PopupWindow(content, 100, 50);
        Log.i(TAG, "popup " + popup.getWidth() + "x" + popup.getHeight() + " showing=" + popup.isShowing()
                + " focusable=" + popup.isFocusable() + " touchable=" + popup.isTouchable() + " outside="
                + popup.isOutsideTouchable() + " clipping=" + popup.isClippingEnabled() + " content="
                + (popup.getContentView() == content));
        popup.setFocusable(true);
        popup.setOutsideTouchable(true);
        popup.setWidth(200);
        popup.setHeight(80);
        popup.setAnimationStyle(0);
        popup.setElevation(4f);
        popup.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        popup.setSoftInputMode(0x10);
        Log.i(TAG, "popup " + popup.getWidth() + "x" + popup.getHeight() + " focusable=" + popup.isFocusable()
                + " outside=" + popup.isOutsideTouchable() + " anim=" + popup.getAnimationStyle() + " elevation="
                + popup.getElevation() + " ime=" + popup.getInputMethodMode() + " soft=" + popup.getSoftInputMode());
        popup.dismiss();
        Log.i(TAG, "popup dismissed showing=" + popup.isShowing());
        // Showing a popup is left out: Minato shows only an activity's own window (ADR-0041 decision 2, an open gap).
    }

    private void listPopup() {
        ListPopupWindow list = new ListPopupWindow(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line,
                new String[] {"a", "b", "c"});
        list.setAdapter(adapter);
        list.setWidth(300);
        list.setHeight(ListPopupWindow.WRAP_CONTENT);
        list.setHorizontalOffset(5);
        list.setVerticalOffset(7);
        list.setModal(true);
        list.setPromptPosition(ListPopupWindow.POSITION_PROMPT_BELOW);
        list.setInputMethodMode(ListPopupWindow.INPUT_METHOD_NEEDED);
        Log.i(TAG, "listpopup " + list.getWidth() + " " + list.getHeight()
                + " " + list.getHorizontalOffset() + " " + list.getVerticalOffset() + " modal=" + list.isModal()
                + " prompt=" + list.getPromptPosition() + " ime=" + list.getInputMethodMode() + " showing="
                + list.isShowing() + " selected=" + list.getSelectedItemPosition() + " list=" + (list.getListView() == null));
        list.dismiss();
        Log.i(TAG, "listpopup dismissed " + list.isShowing());
    }

    private void autoComplete() {
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_dropdown_item_1line,
                new String[] {"alpha", "alpine", "beta", "Alps", "gamma"});
        AutoCompleteTextView view = new AutoCompleteTextView(this);
        Log.i(TAG, "auto threshold=" + view.getThreshold() + " adapter=" + view.getAdapter() + " showing="
                + view.isPopupShowing() + " dropdown=" + view.getDropDownWidth() + "/" + view.getDropDownHeight());
        view.setAdapter(adapter);
        view.setThreshold(2);
        view.setDropDownWidth(400);
        view.setDropDownHorizontalOffset(3);
        view.setDropDownVerticalOffset(4);
        view.setCompletionHint("hint");
        view.setListSelection(0);
        Log.i(TAG, "auto threshold=" + view.getThreshold() + " adapter=" + (view.getAdapter() == adapter)
                + " dropdown=" + view.getDropDownWidth() + " " + view.getDropDownHorizontalOffset() + " "
                + view.getDropDownVerticalOffset() + " hint=" + view.getCompletionHint() + " selection="
                + view.getListSelection() + " enough=" + view.enoughToFilter());
        view.setText("al", false);
        Log.i(TAG, "auto text=" + view.getText() + " enough=" + view.enoughToFilter() + " showing="
                + view.isPopupShowing());
        MultiAutoCompleteTextView multi = new MultiAutoCompleteTextView(this);
        multi.setAdapter(adapter);
        multi.setTokenizer(new MultiAutoCompleteTextView.CommaTokenizer());
        multi.setThreshold(1);
        multi.setText("alpha, be", false);
        multi.setSelection(9);
        Log.i(TAG, "multi text=" + multi.getText() + " enough=" + multi.enoughToFilter());
        Handler main = new Handler(Looper.getMainLooper());
        // The filter chain below answers within milliseconds; the end is a timer, so a stalled filter still ends the run.
        main.postDelayed(() -> Log.i(TAG, "done"), 3000);
        adapter.getFilter().filter("al", count -> {
            Log.i(TAG, "filter al count=" + count + " first=" + (count > 0 ? adapter.getItem(0) : null));
            adapter.getFilter().filter("be", count2 -> {
                Log.i(TAG, "filter be count=" + count2 + " first=" + (count2 > 0 ? adapter.getItem(0) : null));
                adapter.getFilter().filter("", count3 -> {
                    Log.i(TAG, "filter empty count=" + count3);
                    Log.i(TAG, "auto convert=" + adapter.getFilter().convertResultToString("x"));
                });
            });
        });
    }
}
