package io.github.kisame76.minato.test.enter;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import java.util.ArrayList;
import java.util.List;

/**
 * The return key's test (ADR-0068): seven fields, one after another, each of which a keyboard serves as LatinIME does
 * (FixtureInputMethod's keys, which Minato's host types as text records): a plain single-line EditText that only
 * listens for keys, one with the Send action and a listener, one with an action label, a multi-line one, a number
 * field, an engine's view with a full editor and imeOptions 0, and a view without an editor (TYPE_NULL). It logs each
 * field's EditorInfo, each call of the keyboard with the field's text after it, and at the field's end the keys and
 * editor actions that reached it and its text; a field is done after its last key's call, and the focus goes to the
 * next one. The keys and actions wait for the field's end, and a key's call logs no text, since an input method's key
 * events reach the view in a message of their own, which on Android may come before or after the keyboard's next
 * call.
 * The tag is the app's label: enter-test, or its twin enter-test-15 of target SDK 15, before Jelly Bean, whose Enter
 * and delete LatinIME sends as keys.
 */
public class EnterActivity extends Activity {
    static final String PACKAGE = "io.github.kisame76.minato.test.enter";
    /** The fields in their order, each with the keys the keyboard types into it: \n Enter, \b delete. */
    static final String[] FIELDS = {"plain", "send", "label", "multi", "number", "engine", "null"};
    static final String[] KEYS = {"x\n", "y\n", "\n", "a\nb\b", "7\b", "z\n", "5\n\b"};
    private static final long TIMEOUT_MS = 30000;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final View[] mViews = new View[FIELDS.length];
    private String mLogTag = "enter-test";
    private int mField = -1;
    private int mCalls;
    /** The keys and editor actions that reached the field, logged at its end. */
    private final List<String> mKeys = new ArrayList<>();
    private boolean mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mLogTag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < FIELDS.length; i++) {
            mViews[i] = field(i);
            // The keyboard knows the field by EditorInfo.fieldId.
            mViews[i].setId(i + 1);
            root.addView(mViews[i], new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 150));
        }
        setContentView(root);
    }

    private View field(int index) {
        final String name = FIELDS[index];
        switch (name) {
            case "engine":
                return new EngineView(this, name, true, InputType.TYPE_CLASS_TEXT);
            case "null":
                return new EngineView(this, name, false, InputType.TYPE_NULL);
            default:
                LoggingEditText edit = new LoggingEditText(this, name);
                if (name.equals("send")) {
                    edit.setImeOptions(EditorInfo.IME_ACTION_SEND);
                } else if (name.equals("label")) {
                    edit.setImeActionLabel("Join", 42);
                } else if (name.equals("multi")) {
                    edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
                } else if (name.equals("number")) {
                    edit.setInputType(InputType.TYPE_CLASS_NUMBER);
                }
                if (!name.equals("plain")) {
                    edit.setOnEditorActionListener((view, actionId, event) -> {
                        mKeys.add(name + " editor action " + actionId + ", event "
                                + (event == null ? "none" : keyText(event)));
                        return true;
                    });
                }
                edit.setOnKeyListener((view, keyCode, event) -> {
                    mKeys.add(name + " key " + keyText(event));
                    return false;
                });
                return edit;
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && mField < 0) {
            next();
            mHandler.postDelayed(() -> {
                if (!mDone) {
                    Log.i(mLogTag, "timeout in " + FIELDS[mField]);
                    finishRun();
                }
            }, TIMEOUT_MS);
        }
    }

    /** The focus and the keyboard go to the next field, or the run ends after the last. */
    private void next() {
        if (mField >= 0) {
            for (String key : mKeys) {
                Log.i(mLogTag, key);
            }
            mKeys.clear();
            Log.i(mLogTag, FIELDS[mField] + " text \"" + textOf(mViews[mField]) + "\"");
        }
        mField++;
        mCalls = 0;
        if (mField == FIELDS.length) {
            finishRun();
            return;
        }
        View view = mViews[mField];
        view.requestFocus();
        show(view, 10);
    }

    /**
     * Shows the keyboard for `view`, again a little later while the manager refuses: an app of target SDK 15 gets its
     * window's focus before the manager serves the view.
     */
    private void show(View view, int attempts) {
        if (!getSystemService(InputMethodManager.class).showSoftInput(view, 0) && attempts > 1) {
            mHandler.postDelayed(() -> show(view, attempts - 1), 200);
        }
    }

    /** A call of the keyboard on field `name` (a key's down not counted, as its up follows). */
    private void called(String name, String call, boolean result, boolean counts) {
        int index = mField;
        Log.i(mLogTag, name + " call " + call + " -> " + result
                + (call.startsWith("sendKeyEvent") ? "" : ", text \"" + textOf(mViews[index]) + "\""));
        if (counts && FIELDS[index].equals(name) && ++mCalls == KEYS[index].length()) {
            mHandler.post(this::next);
        }
    }

    private void editor(String name, EditorInfo info) {
        Log.i(mLogTag, name + " editor: inputType 0x" + Integer.toHexString(info.inputType) + ", imeOptions 0x"
                + Integer.toHexString(info.imeOptions) + ", actionId " + info.actionId + ", actionLabel "
                + info.actionLabel);
    }

    private static String textOf(View view) {
        CharSequence text = view instanceof EditText ? ((EditText) view).getText()
                : view instanceof EngineView ? ((EngineView) view).mText : "";
        return text.toString().replace("\n", "\\n");
    }

    static String keyText(KeyEvent event) {
        return KeyEvent.keyCodeToString(event.getKeyCode()) + " "
                + (event.getAction() == KeyEvent.ACTION_DOWN ? "down" : "up") + " flags 0x"
                + Integer.toHexString(event.getFlags());
    }

    private void finishRun() {
        if (mDone) {
            return;
        }
        mDone = true;
        Log.i(mLogTag, "done");
    }

    /** An EditText that logs the calls on its connection. */
    private class LoggingEditText extends EditText {
        private final String mName;
        private boolean mLogged;

        LoggingEditText(Context context, String name) {
            super(context);
            mName = name;
            setSingleLine(true);
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            InputConnection target = super.onCreateInputConnection(outAttrs);
            if (!mLogged) {
                mLogged = true;
                editor(mName, outAttrs);
            }
            return target == null ? null : new LoggingConnection(target, mName);
        }
    }

    private class LoggingConnection extends InputConnectionWrapper {
        private final String mName;

        LoggingConnection(InputConnection target, String name) {
            super(target, true);
            mName = name;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            boolean result = super.commitText(text, newCursorPosition);
            called(mName, "commitText \"" + text.toString().replace("\n", "\\n") + "\" " + newCursorPosition, result,
                    true);
            return result;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            boolean result = super.deleteSurroundingText(beforeLength, afterLength);
            called(mName, "deleteSurroundingText " + beforeLength + "," + afterLength, result, true);
            return result;
        }

        @Override
        public boolean performEditorAction(int editorAction) {
            boolean result = super.performEditorAction(editorAction);
            called(mName, "performEditorAction " + editorAction, result, true);
            return result;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            boolean result = super.sendKeyEvent(event);
            called(mName, "sendKeyEvent " + keyText(event), result, event.getAction() == KeyEvent.ACTION_UP);
            return result;
        }
    }

    /**
     * A view that takes text without being a TextView, as a game engine's: a BaseInputConnection with a full editor of
     * its own and imeOptions 0, or, without an editor, TYPE_NULL. It logs the keys that reach it.
     */
    private class EngineView extends View {
        final Editable mText = Editable.Factory.getInstance().newEditable("");
        private final String mName;
        private final boolean mFullEditor;
        private final int mInputType;
        private boolean mLogged;

        EngineView(Context context, String name, boolean fullEditor, int inputType) {
            super(context);
            mName = name;
            mFullEditor = fullEditor;
            mInputType = inputType;
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = mInputType;
            outAttrs.imeOptions = 0;
            if (!mLogged) {
                mLogged = true;
                editor(mName, outAttrs);
            }
            return new EngineConnection();
        }

        @Override
        public boolean onKeyDown(int keyCode, KeyEvent event) {
            mKeys.add(mName + " key " + keyText(event));
            return true;
        }

        @Override
        public boolean onKeyUp(int keyCode, KeyEvent event) {
            mKeys.add(mName + " key " + keyText(event));
            return true;
        }

        private class EngineConnection extends BaseInputConnection {
            /** A call within one of the keyboard's, such as the keys of performEditorAction's default. */
            private int mDepth;

            EngineConnection() {
                super(EngineView.this, mFullEditor);
            }

            @Override
            public Editable getEditable() {
                return mFullEditor ? mText : super.getEditable();
            }

            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                mDepth++;
                boolean result = super.commitText(text, newCursorPosition);
                mDepth--;
                called(mName, "commitText \"" + text.toString().replace("\n", "\\n") + "\" " + newCursorPosition,
                        result, mDepth == 0);
                return result;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                mDepth++;
                boolean result = super.deleteSurroundingText(beforeLength, afterLength);
                mDepth--;
                called(mName, "deleteSurroundingText " + beforeLength + "," + afterLength, result, mDepth == 0);
                return result;
            }

            @Override
            public boolean performEditorAction(int editorAction) {
                mDepth++;
                boolean result = super.performEditorAction(editorAction);
                mDepth--;
                called(mName, "performEditorAction " + editorAction, result, mDepth == 0);
                return result;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                mDepth++;
                boolean result = super.sendKeyEvent(event);
                mDepth--;
                called(mName, "sendKeyEvent " + keyText(event), result,
                        mDepth == 0 && event.getAction() == KeyEvent.ACTION_UP);
                return result;
            }
        }
    }
}
