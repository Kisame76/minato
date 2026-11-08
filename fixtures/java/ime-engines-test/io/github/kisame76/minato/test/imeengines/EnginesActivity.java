package io.github.kisame76.minato.test.imeengines;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.text.SpannableStringBuilder;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
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
 * ADR-0068's text bar and the text fields of game engines and app frameworks, built as their open sources and public
 * documents describe them (none of their classes), each holding text before the keyboard comes. In turn:
 * <ul>
 * <li>"godot": a hidden EditText that the engine empties, appends the field's text to, selects at its end, focuses
 *     and shows the keyboard for, as Godot's GodotEditText does;
 * <li>"flutter": a view whose connection is a BaseInputConnection on an editable that the framework set (Flutter's
 *     TextInputPlugin setEditingState, its InputConnectionAdaptor), with the editor's initial selection;
 * <li>"cocos": an EditText laid over the game with its text set, then focused, as Cocos2d-x's EditBox does;
 * <li>"dialog": an AlertDialog whose EditText holds the text and asks for the keyboard with its window's soft input
 *     state, as a game engine's text input dialog does (libGDX's getTextInput; a TouchScreenKeyboard opened with
 *     a text); then the same dialog with the other modes Android decides the keyboard by: "dialog-visible",
 *     "dialog-resize" and "dialog-plain".
 * </ul>
 * SDL2 and SDL3 hand a keyboard no text of the game's own (ime-prefill-test's "sdl"). Each connection logs the first
 * answer of each kind it gives the input method, with its number for its field, and each field whether the keyboard
 * is up for it when the next one comes. The run ends with "done".
 */
public class EnginesActivity extends Activity {
    private static final String TAG = "ime-engines-test";
    private static final long STEP_MS = 2500;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mAt;
    private LoggingEditText mGodot;
    private FrameworkView mFlutter;
    private LoggingEditText mCocos;
    private AlertDialog mDialog;
    // The field of the step that runs, and the window it is in, whose keyboard the next step logs.
    private String mName;
    private View mShown;
    // The next dialog, which opens once the activity has the focus back from the last one.
    private Runnable mAfterDialog;

    /** An EditText whose connections log. */
    private static final class LoggingEditText extends EditText {
        private final String mName;
        private int mConnections;

        LoggingEditText(Context context, String name, int imeOptions) {
            super(context);
            mName = name;
            setInputType(InputType.TYPE_CLASS_TEXT);
            setImeOptions(imeOptions);
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            final InputConnection target = super.onCreateInputConnection(outAttrs);
            return target == null ? null : new Logging(target, mName + " #" + ++mConnections, outAttrs);
        }
    }

    /** A framework's own text view: its editing state lives in an editable that the framework sets. */
    private static final class FrameworkView extends View {
        private final Editable mEditable = new SpannableStringBuilder();
        private int mConnections;

        FrameworkView(Context context) {
            super(context);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        /** The framework's setEditingState: the text and its selection. */
        void setEditingState(String text) {
            mEditable.replace(0, mEditable.length(), text);
            Selection.setSelection(mEditable, text.length());
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT;
            outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            outAttrs.initialSelStart = Selection.getSelectionStart(mEditable);
            outAttrs.initialSelEnd = Selection.getSelectionEnd(mEditable);
            final BaseInputConnection connection = new BaseInputConnection(this, true) {
                @Override
                public Editable getEditable() {
                    return mEditable;
                }
            };
            return new Logging(connection, "flutter #" + ++mConnections, outAttrs);
        }
    }

    /** Logs the first answer of each kind, and the editor it serves. */
    private static final class Logging extends InputConnectionWrapper {
        private final String mName;
        private final Set<String> mLogged = new HashSet<>();

        Logging(InputConnection target, String name, EditorInfo info) {
            super(target, true);
            mName = name;
            Log.i(TAG, name + ": editor selection " + info.initialSelStart + "," + info.initialSelEnd);
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
        mGodot = new LoggingEditText(this, "godot",
                EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_ACTION_DONE);
        mFlutter = new FrameworkView(this);
        mCocos = new LoggingEditText(this, "cocos", EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_DONE);
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        for (View field : new View[] {mGodot, mFlutter, mCocos}) {
            column.addView(field, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 200));
        }
        setContentView(column);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && mAfterDialog != null) {
            // After the input method manager has heard of the focus, as each run then has the same order.
            mHandler.post(mAfterDialog);
            mAfterDialog = null;
            return;
        }
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        Log.i(TAG, "ready");
        step();
    }

    private void step() {
        if (mShown != null) {
            final WindowInsets insets = mShown.getRootWindowInsets();
            Log.i(TAG, mName + ": keyboard " + (insets != null && insets.isVisible(WindowInsets.Type.ime())));
        }
        final InputMethodManager keyboard = getSystemService(InputMethodManager.class);
        mShown = getWindow().getDecorView();
        switch (mAt++) {
            case 0:
                mName = "godot";
                mGodot.setText("");
                mGodot.append("Godette");
                mGodot.setSelection(mGodot.length());
                mGodot.requestFocus();
                keyboard.showSoftInput(mGodot, 0);
                break;
            case 1:
                mName = "flutter";
                mFlutter.setEditingState("Dash");
                mFlutter.requestFocus();
                keyboard.showSoftInput(mFlutter, 0);
                break;
            case 2:
                mName = "cocos";
                mCocos.setText("Cocos");
                mCocos.setSelection(mCocos.length());
                mCocos.requestFocus();
                keyboard.showSoftInput(mCocos, 0);
                break;
            // The dialogs last: they ask for the keyboard with their window's soft input mode, not showSoftInput, the
            // keyboard hidden before them.
            case 3:
                dialog("dialog", WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE, false);
                break;
            case 4:
                dialog("dialog-visible", WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE, false);
                break;
            case 5:
                // No state, a window that resizes for the keyboard: Android shows it for the editor going forward.
                dialog("dialog-resize", WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE, false);
                break;
            case 6:
                // No mode at all, the keyboard up before it: the window pans, and Android hides the keyboard.
                dialog("dialog-plain", WindowManager.LayoutParams.SOFT_INPUT_STATE_UNSPECIFIED, true);
                break;
            default:
                Log.i(TAG, "done");
                return;
        }
        mHandler.postDelayed(this::step, STEP_MS);
    }

    /**
     * An AlertDialog whose EditText holds a name, its window with soft input mode `mode`, once the activity has the
     * focus back from the last dialog; just before it the keyboard is shown for the activity's field if `shown`,
     * hidden otherwise.
     */
    private void dialog(String name, int mode, boolean shown) {
        mName = name;
        mShown = null;
        final Runnable open = () -> {
            final InputMethodManager keyboard = getSystemService(InputMethodManager.class);
            if (shown) {
                keyboard.showSoftInput(mCocos, 0);
            } else {
                keyboard.hideSoftInputFromWindow(getWindow().getDecorView().getWindowToken(), 0);
            }
            final LoggingEditText input = new LoggingEditText(this, name, EditorInfo.IME_ACTION_DONE);
            input.setText("Player 1");
            input.setSelection(input.length());
            mDialog = new AlertDialog.Builder(this).setTitle("Name").setView(input).setPositiveButton("OK", null)
                    .create();
            if (mode != WindowManager.LayoutParams.SOFT_INPUT_STATE_UNSPECIFIED) {
                mDialog.getWindow().setSoftInputMode(mode);
            }
            mDialog.show();
            input.requestFocus();
            mShown = mDialog.getWindow().getDecorView();
        };
        if (mDialog == null) {
            open.run();
            return;
        }
        mDialog.dismiss();
        mDialog = null;
        mAfterDialog = open;
    }
}
