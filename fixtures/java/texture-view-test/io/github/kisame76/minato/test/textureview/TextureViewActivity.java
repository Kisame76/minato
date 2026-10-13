package io.github.kisame76.minato.test.textureview;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.SurfaceTexture;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.TextureView;
import android.view.View;
import android.widget.LinearLayout;

import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0058 d)'s test (texture-view-test): two TextureViews of 600 x 400 pixels in a column in the middle of the
 * window, fed by their own lockCanvas with four colored quarters and a square in the middle. The plain one gets two
 * frames, the second after the first is shown; the mirrored one has a transform that mirrors it left to right and gets
 * one. Before the window, the app logs what a TextureView without its SurfaceTexture answers and its refusal of a
 * foreground and a background. Each view's listener calls, with getBitmap's pixels at the quarters' middles (full size
 * and half size) once a frame is shown, are kept per view and logged in a fixed order at the end, so that the log does
 * not depend on whether both views' frames come in one draw. The views' bounds in the window are logged once laid out;
 * the check compares the screen's pixels there with Android's. "done" comes after the draw that shows the plain view's
 * second frame.
 */
public class TextureViewActivity extends Activity {
    private static final String TAG = "texture-view-test";
    private static final int WIDTH = 600;
    private static final int HEIGHT = 400;
    private static final int[][] COLORS = {
            {0xFFD02020, 0xFF20A040, 0xFF2040D0, 0xFFF0C020, 0xFF101820},
            {0xFF40C0C0, 0xFFC040C0, 0xFF808020, 0xFF603010, 0xFFF0F0F0},
    };

    private LinearLayout mColumn;
    private TextureView mPlain;
    private TextureView mMirrored;
    private final List<String> mPlainLines = new ArrayList<>();
    private final List<String> mMirroredLines = new ArrayList<>();
    private int mPlainFrames;
    private int mMirroredFrames;
    private boolean mLaidOut;
    private boolean mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setBackgroundDrawable(new ColorDrawable(0xFFE8EEF4));
        mColumn = new LinearLayout(this);
        mColumn.setOrientation(LinearLayout.VERTICAL);
        mColumn.setGravity(Gravity.CENTER);

        mPlain = new TextureView(this);
        Log.i(TAG, "before the window: available " + mPlain.isAvailable() + ", surface texture "
                + mPlain.getSurfaceTexture() + ", lockCanvas " + mPlain.lockCanvas() + ", layer type "
                + mPlain.getLayerType() + ", opaque " + mPlain.isOpaque());
        try {
            mPlain.setForeground(new ColorDrawable(0xFF000000));
            Log.i(TAG, "setForeground: taken");
        } catch (RuntimeException e) {
            Log.i(TAG, "setForeground: " + e);
        }
        try {
            mPlain.setBackground(new ColorDrawable(0xFF000000));
            Log.i(TAG, "setBackground: taken");
        } catch (RuntimeException e) {
            Log.i(TAG, "setBackground: " + e);
        }
        mPlain.setSurfaceTextureListener(new Listener(mPlain, mPlainLines, true));

        mMirrored = new TextureView(this);
        Matrix mirror = new Matrix();
        mirror.setScale(-1, 1, WIDTH / 2f, HEIGHT / 2f);
        mMirrored.setTransform(mirror);
        Log.i(TAG, "mirrored transform: " + mMirrored.getTransform(null).toShortString());
        mMirrored.setSurfaceTextureListener(new Listener(mMirrored, mMirroredLines, false));

        mColumn.addView(mPlain, new LinearLayout.LayoutParams(WIDTH, HEIGHT));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(WIDTH, HEIGHT);
        params.topMargin = 48;
        mColumn.addView(mMirrored, params);
        setContentView(mColumn);
        mColumn.getViewTreeObserver().addOnGlobalLayoutListener(this::laidOut);
    }

    private void laidOut() {
        if (mLaidOut || mPlain.getWidth() == 0) {
            return;
        }
        mLaidOut = true;
        Log.i(TAG, "plain bounds " + bounds(mPlain));
        Log.i(TAG, "mirrored bounds " + bounds(mMirrored));
    }

    /** A frame through lockCanvas: four quarters and a square of 100 pixels in the middle in the frame's colors. */
    private static String draw(TextureView view, int frame) {
        Canvas canvas = view.lockCanvas();
        if (canvas == null) {
            return "frame " + frame + ": lockCanvas null";
        }
        int width = canvas.getWidth();
        int height = canvas.getHeight();
        int[] colors = COLORS[frame - 1];
        Paint paint = new Paint();
        paint.setColor(colors[0]);
        canvas.drawRect(0, 0, width / 2f, height / 2f, paint);
        paint.setColor(colors[1]);
        canvas.drawRect(width / 2f, 0, width, height / 2f, paint);
        paint.setColor(colors[2]);
        canvas.drawRect(0, height / 2f, width / 2f, height, paint);
        paint.setColor(colors[3]);
        canvas.drawRect(width / 2f, height / 2f, width, height, paint);
        paint.setColor(colors[4]);
        canvas.drawRect(width / 2f - 50, height / 2f - 50, width / 2f + 50, height / 2f + 50, paint);
        view.unlockCanvasAndPost(canvas);
        return "frame " + frame + ": canvas " + width + " x " + height;
    }

    /** The bitmap's size and its pixels at the middles of its quarters and its own middle. */
    private static String pixels(Bitmap bitmap) {
        if (bitmap == null) {
            return "null";
        }
        int width = bitmap.getWidth();
        int height = bitmap.getHeight();
        return width + " x " + height + " " + bitmap.getConfig() + String.format(" %08x %08x %08x %08x %08x",
                bitmap.getPixel(width / 4, height / 4), bitmap.getPixel(3 * width / 4, height / 4),
                bitmap.getPixel(width / 4, 3 * height / 4), bitmap.getPixel(3 * width / 4, 3 * height / 4),
                bitmap.getPixel(width / 2, height / 2));
    }

    /** The plain view's second frame is shown: both views' lines, then "done". */
    private void finish(String last) {
        if (mDone) {
            return;
        }
        mDone = true;
        for (String line : mPlainLines) {
            Log.i(TAG, "plain: " + line);
        }
        for (String line : mMirroredLines) {
            Log.i(TAG, "mirrored: " + line);
        }
        Log.i(TAG, last);
        Log.i(TAG, "done");
    }

    private static String bounds(View view) {
        int[] location = new int[2];
        view.getLocationInWindow(location);
        return location[0] + " " + location[1] + " " + (location[0] + view.getWidth()) + " "
                + (location[1] + view.getHeight());
    }

    private final class Listener implements TextureView.SurfaceTextureListener {
        private final TextureView mView;
        private final List<String> mLines;
        private final boolean mPlainView;

        Listener(TextureView view, List<String> lines, boolean plain) {
            mView = view;
            mLines = lines;
            mPlainView = plain;
        }

        @Override
        public void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height) {
            mLines.add("available: " + width + " x " + height + ", isAvailable " + mView.isAvailable()
                    + ", getSurfaceTexture the same " + (mView.getSurfaceTexture() == surface) + ", on the main thread "
                    + (Looper.myLooper() == Looper.getMainLooper()));
            mLines.add(draw(mView, 1));
        }

        @Override
        public void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height) {
            mLines.add("size changed: " + width + " x " + height);
        }

        @Override
        public void onSurfaceTextureUpdated(SurfaceTexture surface) {
            int frame = mPlainView ? ++mPlainFrames : ++mMirroredFrames;
            mLines.add("updated " + frame + ", timestamp set " + (surface.getTimestamp() > 0));
            // getBitmap is not for a drawing method, which this call is in: after the draw.
            mView.post(() -> {
                mLines.add("frame " + frame + " shown: getBitmap " + pixels(mView.getBitmap()) + ", half "
                        + pixels(mView.getBitmap(WIDTH / 2, HEIGHT / 2)));
                if (mPlainView && frame == 1) {
                    mLines.add(draw(mView, 2));
                } else if (mPlainView && frame == 2) {
                    finish("mirrored frames " + mMirroredFrames);
                }
            });
        }

        @Override
        public boolean onSurfaceTextureDestroyed(SurfaceTexture surface) {
            mLines.add("destroyed");
            return true;
        }
    }
}
