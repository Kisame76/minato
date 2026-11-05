package io.github.kisame76.minato.test.magnifier;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.view.PixelCopy;
import android.view.View;

import java.util.Locale;

// The text selection's loupe draws its content from a PixelCopy of the window (android.widget.Magnifier does), and the
// view it magnifies draws in software. This app fills its window with a known color, drawn in software, copies the
// window into a small bitmap once the window is shown, and logs the copy's result and the bitmap's center pixel, then
// "done". On Android, and on Minato once HardwareRenderer.nCopySurfaceInto reads a window's last posted software
// buffer (ADR-0043 amendment), the copy succeeds and the center is the color; before that Minato answers
// PixelCopy.ERROR_SOURCE_NO_DATA, so the loupe is empty.
public class MagnifierActivity extends Activity {
    private static final int FILL = 0xFF3366CC;
    private String tag;
    private boolean copied;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        tag = getPackageManager().getApplicationLabel(getApplicationInfo()).toString();
        final View content = new View(this);
        content.setBackgroundColor(FILL);
        setContentView(content);
        getWindow().setBackgroundDrawable(new ColorDrawable(FILL));
        Log.i(tag, "created");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || copied) {
            return;
        }
        copied = true;
        final HandlerThread thread = new HandlerThread("pixelcopy");
        thread.start();
        final Handler handler = new Handler(thread.getLooper());
        // After a frame has been drawn and posted, so the window has a buffer to read back.
        getWindow().getDecorView().postDelayed(() -> copy(handler), 500);
    }

    private void copy(Handler handler) {
        final Bitmap bitmap = Bitmap.createBitmap(40, 40, Bitmap.Config.ARGB_8888);
        PixelCopy.request(getWindow(), bitmap, result -> {
            Log.i(tag, "pixelcopy result " + result);
            if (result == PixelCopy.SUCCESS) {
                Log.i(tag, "pixelcopy center " + String.format(Locale.ROOT, "%08x", bitmap.getPixel(20, 20)));
            }
            Log.i(tag, "done");
            finish();
        }, handler);
    }
}
