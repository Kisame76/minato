package io.github.kisame76.minato.test.imeprefill;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import java.util.HashSet;
import java.util.Set;

/**
 * ADR-0068's text bar and the text a field holds before the keyboard comes. Four fields take the focus in turn, each
 * with showSoftInput: "before", an EditText whose text is set before its focus; "while", an EditText whose text is set
 * a second after its focus (TextView restarts the input then); "hint", an empty EditText with a hint; and "sdl", a view
 * as SDL2's DummyEdit, whose connection has an editable of its own that starts empty while the app draws a default
 * name itself, as Ren'Py does. Each connection logs the first answer of each kind it gives the input method
 * (getExtractedText, getTextBeforeCursor, getSelectedText, getTextAfterCursor), with the connection's number for its
 * field, and the editor's initial selection and hint. The run ends with "done".
 */
public class PrefillActivity extends Activity {
    private static final String TAG = "ime-prefill-test";
    private static final long STEP_MS = 2500;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private View[] mFields;
    private int mAt;

    /** An EditText whose connections log. */
    private final class Field extends EditText {
        private final String mName;
        private int mConnections;

        Field(Context context, String name) {
            super(context);
            mName = name;
            setInputType(InputType.TYPE_CLASS_TEXT);
            setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            final InputConnection target = super.onCreateInputConnection(outAttrs);
            if (target == null) {
                return null;
            }
            return new Logging(target, mName + " #" + ++mConnections, outAttrs);
        }
    }

    /** As SDL2's DummyEdit: a text editor with a BaseInputConnection that has an editable of its own. */
    private final class DummyEdit extends View {
        private int mConnections;

        DummyEdit(Context context) {
            super(context);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT;
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            return new Logging(new BaseInputConnection(this, true), "sdl #" + ++mConnections, outAttrs);
        }
    }

    /** Logs the first answer of each kind, and the editor it serves. */
    private static final class Logging extends InputConnectionWrapper {
        private final String mName;
        private final Set<String> mLogged = new HashSet<>();

        Logging(InputConnection target, String name, EditorInfo info) {
            super(target, true);
            mName = name;
            Log.i(TAG, name + ": editor selection " + info.initialSelStart + "," + info.initialSelEnd + ", hint "
                    + (info.hintText == null ? "none" : "\"" + info.hintText + "\""));
        }

        private synchronized void first(String kind, CharSequence answer) {
            if (mLogged.add(kind)) {
                Log.i(TAG, mName + ": " + kind + " -> " + (answer == null ? "null" : "\"" + answer + "\""));
            }
        }

        @Override
        public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) {
            final ExtractedText result = super.getExtractedText(request, flags);
            first("getExtractedText", result == null ? null : result.text);
            return result;
        }

        @Override
        public CharSequence getTextBeforeCursor(int length, int flags) {
            final CharSequence result = super.getTextBeforeCursor(length, flags);
            first("getTextBeforeCursor", result);
            return result;
        }

        @Override
        public CharSequence getSelectedText(int flags) {
            final CharSequence result = super.getSelectedText(flags);
            first("getSelectedText", result);
            return result;
        }

        @Override
        public CharSequence getTextAfterCursor(int length, int flags) {
            final CharSequence result = super.getTextAfterCursor(length, flags);
            first("getTextAfterCursor", result);
            return result;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final Field before = new Field(this, "before");
        before.setText("Eileen");
        before.setSelection(before.length());
        final Field whileFocused = new Field(this, "while");
        final Field hint = new Field(this, "hint");
        hint.setHint("Your name");
        mFields = new View[] {before, whileFocused, hint, new DummyEdit(this)};
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        for (View field : mFields) {
            column.addView(field, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 200));
        }
        setContentView(column);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        Log.i(TAG, "ready");
        focusNext();
    }

    private void focusNext() {
        if (mAt == mFields.length) {
            Log.i(TAG, "done");
            return;
        }
        final View field = mFields[mAt++];
        field.requestFocus();
        getSystemService(InputMethodManager.class).showSoftInput(field, 0);
        if (mAt == 2) {
            // "while": the app sets the text once the keyboard serves the field, as a game fills in a name.
            mHandler.postDelayed(() -> {
                final EditText text = (EditText) field;
                text.setText("Lucy");
                text.setSelection(text.length());
                Log.i(TAG, "while: text set");
            }, 1000);
        }
        mHandler.postDelayed(this::focusNext, STEP_MS);
    }
}
