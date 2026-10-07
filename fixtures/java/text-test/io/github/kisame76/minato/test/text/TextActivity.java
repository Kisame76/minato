package io.github.kisame76.minato.test.text;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.Layout;
import android.text.TextWatcher;
import android.text.method.DigitsKeyListener;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * ADR-0043 step d)'s test: an EditText made as Godot's onInitRenderView makes its GodotEditText, beside a TextView. It
 * logs the EditText's defaults, Godot's setup with its filter and key listeners, each TextWatcher call, both views
 * after the global layout, then, at the window's first focus, typing as an input method types through an
 * InputConnection, the input method manager's answers on a device without an input method, and both again after another
 * view took the focus while a text was composed. The TextView's color, from the system palette, is compared since
 * ADR-0043 step f), which gives Minato Android 15's Monet overlays.
 */
public class TextActivity extends Activity {
    private static final String TAG = "text-test";
    private GodotLikeEditText mEdit;
    private TextView mText;
    private View mOther;
    private boolean mFocused;

    /** As GodotEditText: its padding and IME options from its constructor (initView). */
    static class GodotLikeEditText extends EditText {
        GodotLikeEditText(Context context) {
            super(context);
            setPadding(0, 0, 0, 0);
            setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_DONE);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mEdit = new GodotLikeEditText(this);
        mEdit.setId(1);
        Log.i(TAG, "edit: " + describe(mEdit) + ", color 0x" + Integer.toHexString(mEdit.getCurrentTextColor()));
        mText = new TextView(this);
        mText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        mText.setText("Minato text");
        Log.i(TAG, "text: " + describe(mText));
        Log.i(TAG, "text: color 0x" + Integer.toHexString(mText.getCurrentTextColor()));

        mEdit.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
                Log.i(TAG, "before \"" + s + "\" " + start + " " + count + " " + after);
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                Log.i(TAG, "on \"" + s + "\" " + start + " " + before + " " + count);
            }

            @Override
            public void afterTextChanged(Editable s) {
                Log.i(TAG, "after \"" + s + "\"");
            }
        });
        mEdit.setOnEditorActionListener((view, actionId, event) -> {
            Log.i(TAG, "editor action " + actionId + ", event " + event);
            return true;
        });

        // Godot's setup: a length filter, the text, a selection and its key listeners for a number keyboard.
        mEdit.setFilters(new InputFilter[] {new InputFilter.LengthFilter(8)});
        mEdit.setText("12345");
        mEdit.append("6789");
        mEdit.setSelection(2, 4);
        Log.i(TAG, "set up: \"" + mEdit.getText() + "\", length " + mEdit.length() + ", selection "
                + mEdit.getSelectionStart() + " " + mEdit.getSelectionEnd());
        mEdit.setKeyListener(DigitsKeyListener.getInstance("0123456789"));
        Log.i(TAG, "digits: " + keys(mEdit));
        mEdit.setKeyListener(DigitsKeyListener.getInstance(Locale.US, true, true));
        Log.i(TAG, "signed decimal: " + keys(mEdit));
        mEdit.setInputType(InputType.TYPE_CLASS_TEXT);
        Log.i(TAG, "text input: " + keys(mEdit));

        FrameLayout root = new FrameLayout(this);
        int height = Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 48,
                getResources().getDisplayMetrics()));
        mEdit.setLayoutParams(new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, height));
        mEdit.setBackgroundColor(Color.TRANSPARENT);
        root.addView(mEdit);
        root.addView(mText, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        // A plain view that can take the focus in touch mode, which has no input connection.
        mOther = new View(this);
        mOther.setFocusableInTouchMode(true);
        root.addView(mOther, new FrameLayout.LayoutParams(height, height, Gravity.BOTTOM));
        setContentView(root);
        root.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                root.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                Log.i(TAG, "layout: edit " + bounds(mEdit) + ", first line's width " + width(mEdit) + "; text "
                        + bounds(mText) + ", first line's width " + width(mText));
            }
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mFocused) {
            return;
        }
        mFocused = true;
        mEdit.setText("");
        Log.i(TAG, "focus: requested " + mEdit.requestFocus() + ", has " + mEdit.hasFocus());
        InputConnection connection = type();
        manager();
        moveFocus(connection);
        Log.i(TAG, "done");
    }

    /** Typing as an input method types, through the EditText's InputConnection. */
    private InputConnection type() {
        EditorInfo info = new EditorInfo();
        InputConnection connection = mEdit.onCreateInputConnection(info);
        Log.i(TAG, "editor info: input type 0x" + Integer.toHexString(info.inputType) + ", ime options 0x"
                + Integer.toHexString(info.imeOptions) + ", selection " + info.initialSelStart + " "
                + info.initialSelEnd + ", field " + info.fieldId + ", package " + info.packageName
                + ", connection " + (connection == null ? null : connection.getClass().getName()));
        connection.beginBatchEdit();
        connection.setComposingText("ab", 1);
        Log.i(TAG, "composing: \"" + mEdit.getText() + "\", span " + composing());
        connection.finishComposingText();
        Log.i(TAG, "finished: \"" + mEdit.getText() + "\", span " + composing());
        connection.commitText("cdefghij", 1);
        Log.i(TAG, "committed: \"" + mEdit.getText() + "\", selection " + mEdit.getSelectionStart() + " "
                + mEdit.getSelectionEnd());
        connection.deleteSurroundingText(2, 0);
        Log.i(TAG, "deleted: \"" + mEdit.getText() + "\", before the cursor \""
                + connection.getTextBeforeCursor(3, 0) + "\"");
        connection.endBatchEdit();
        Log.i(TAG, "editor action sent: " + connection.performEditorAction(EditorInfo.IME_ACTION_DONE));
        return connection;
    }

    /**
     * Another view takes the focus while the input method composes: the manager serves that view at its next check,
     * which finishes the composing text and closes the EditText's connection (InputMethodManager.java:3530-3555,
     * RemoteInputConnectionImpl.java:356-402).
     */
    private void moveFocus(InputConnection connection) {
        connection.setComposingText("kl", 1);
        Log.i(TAG, "composing again: \"" + mEdit.getText() + "\", span " + composing());
        Log.i(TAG, "other focus: requested " + mOther.requestFocus() + ", edit has " + mEdit.hasFocus() + ", span "
                + composing());
        manager();
        Log.i(TAG, "checked: \"" + mEdit.getText() + "\", span " + composing());
    }

    /** What the input method manager answers on a device without an input method. */
    private void manager() {
        InputMethodManager manager = getSystemService(InputMethodManager.class);
        Log.i(TAG, "manager " + manager.getClass().getName() + ": show " + manager.showSoftInput(mEdit, 0)
                + ", active " + manager.isActive() + ", edit active " + manager.isActive(mEdit) + ", accepting "
                + manager.isAcceptingText() + ", fullscreen " + manager.isFullscreenMode() + ", hide "
                + manager.hideSoftInputFromWindow(mEdit.getWindowToken(), 0) + ", methods "
                + manager.getEnabledInputMethodList().size());
    }

    private String composing() {
        Editable text = mEdit.getText();
        return BaseInputConnection.getComposingSpanStart(text) + " " + BaseInputConnection.getComposingSpanEnd(text);
    }

    private static String describe(TextView view) {
        return "input type 0x" + Integer.toHexString(view.getInputType()) + ", ime options 0x"
                + Integer.toHexString(view.getImeOptions()) + ", key listener " + name(view.getKeyListener())
                + ", movement " + name(view.getMovementMethod()) + ", focusable " + view.isFocusable()
                + ", in touch mode " + view.isFocusableInTouchMode() + ", filters " + view.getFilters().length
                + ", text size " + view.getTextSize() + ", max lines " + view.getMaxLines() + ", gravity 0x"
                + Integer.toHexString(view.getGravity()) + ", editable " + view.onCheckIsTextEditor()
                + ", bounds for width " + view.getUseBoundsForWidth();
    }

    private static String keys(TextView view) {
        return "input type 0x" + Integer.toHexString(view.getInputType()) + ", key listener "
                + name(view.getKeyListener()) + ", \"" + view.getText() + "\"";
    }

    private static String bounds(TextView view) {
        return view.getLeft() + " " + view.getTop() + " " + view.getRight() + " " + view.getBottom() + ", lines "
                + view.getLineCount() + ", baseline " + view.getBaseline();
    }

    private static String width(TextView view) {
        Layout layout = view.getLayout();
        return String.valueOf(layout == null ? null : layout.getLineWidth(0));
    }

    private static String name(Object object) {
        return object == null ? null : object.getClass().getName();
    }
}
