package io.github.kisame76.minato.test.imeprobe;

import android.inputmethodservice.InputMethodService;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;

/**
 * The fixture input method of ime-probe-test: a keyboard that logs what an app's field tells it, under the tag
 * ime-probe, one line each: the editor info (input type, variation, flags, IME options, action) when input starts,
 * when the input view shows and hides, when input ends, the window being shown or hidden, and, for a field of another
 * app, the extracted text it answers (plain or masked) after the keyboard has typed "abc" into it.
 */
public class ProbeInputMethod extends InputMethodService {
    private static final String TAG = "ime-probe";
    private final Handler mHandler = new Handler(Looper.getMainLooper());

    @Override
    public boolean onEvaluateFullscreenMode() {
        return false;
    }

    private static String describe(EditorInfo info) {
        if (info == null) {
            return "no editor info";
        }
        int variation = info.inputType & EditorInfo.TYPE_MASK_VARIATION;
        return "package " + info.packageName + " inputType 0x" + Integer.toHexString(info.inputType) + " class "
                + (info.inputType & EditorInfo.TYPE_MASK_CLASS) + " variation " + variation + " imeOptions 0x"
                + Integer.toHexString(info.imeOptions) + " action " + (info.imeOptions & EditorInfo.IME_MASK_ACTION)
                + " hint " + info.hintText + " label " + info.label + " initialSel " + info.initialSelStart + ","
                + info.initialSelEnd;
    }

    private static boolean other(EditorInfo info) {
        return info != null && info.packageName != null && !info.packageName.startsWith("android")
                && !info.packageName.startsWith("com.android");
    }

    @Override
    public void onStartInput(EditorInfo info, boolean restarting) {
        super.onStartInput(info, restarting);
        if (other(info)) {
            Log.i(TAG, "onStartInput restarting " + restarting + " " + describe(info));
        }
    }

    @Override
    public void onStartInputView(EditorInfo info, boolean restarting) {
        super.onStartInputView(info, restarting);
        if (!other(info)) {
            return;
        }
        Log.i(TAG, "onStartInputView restarting " + restarting);
        if (restarting) {
            return;
        }
        final InputConnection connection = getCurrentInputConnection();
        mHandler.postDelayed(() -> {
            connection.commitText("abc", 1);
            mHandler.postDelayed(() -> {
                ExtractedTextRequest request = new ExtractedTextRequest();
                request.token = 1;
                request.hintMaxChars = 4096;
                ExtractedText extracted = connection.getExtractedText(request, 0);
                String text = extracted == null || extracted.text == null ? null : extracted.text.toString();
                Log.i(TAG, "getExtractedText after typing abc: " + (text == null ? "none" : "length " + text.length()
                        + (text.contains("abc") ? " plain" : " masked as " + (text.isEmpty() ? "empty"
                                : "U+" + Integer.toHexString(text.charAt(0))))));
            }, 300);
        }, 700);
    }

    @Override
    public void onFinishInputView(boolean finishingInput) {
        Log.i(TAG, "onFinishInputView finishing " + finishingInput);
        super.onFinishInputView(finishingInput);
    }

    @Override
    public void onFinishInput() {
        Log.i(TAG, "onFinishInput");
        super.onFinishInput();
    }

    @Override
    public void onWindowShown() {
        Log.i(TAG, "onWindowShown");
        super.onWindowShown();
    }

    @Override
    public void onWindowHidden() {
        Log.i(TAG, "onWindowHidden");
        super.onWindowHidden();
    }
}
