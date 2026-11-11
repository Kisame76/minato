package io.github.kisame76.minato.test.surfaceviewdecor;

import android.graphics.Canvas;
import android.view.SurfaceHolder;

/** The software variant: the SurfaceView's own producer posts one opaque CPU frame and stays connected. */
public class SoftwareProducerActivity extends SurfaceDecorActivity {
    @Override
    protected void produce(SurfaceHolder holder) {
        final Canvas canvas = holder.lockCanvas();
        if (canvas != null) {
            canvas.drawColor(0xFF33CC44);
            holder.unlockCanvasAndPost(canvas);
            say("surface posted");
        } else {
            say("surface lock failed");
        }
    }
}
