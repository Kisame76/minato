package io.github.kisame76.minato.test.touch;

import android.app.Application;
import android.graphics.Matrix;
import android.os.Parcel;
import android.util.Log;
import android.view.GestureDetector;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.VelocityTracker;
import android.widget.LinearLayout;

import java.util.Locale;

/**
 * ADR-0045 step 7b1's test of touch time, in Application.onCreate, with made-up events only, so that each line is
 * the same on any device:
 * <ul>
 *   <li>MotionEvent.obtain's getters for a DOWN;
 *   <li>a MOVE with two batched samples: its history;
 *   <li>a second pointer's POINTER_DOWN, and a row of two views that splits a two-finger touch between them;
 *   <li>offsetLocation, transform with a Matrix, setLocation, and the raw coordinates;
 *   <li>the axis labels;
 *   <li>a parcel round trip with its size;
 *   <li>VelocityTracker and GestureDetector's fling over a made-up swipe, the velocities rounded to 0.1 px/s. Its
 *       steps of 40 px pass the touch slop and its 5,000 px/s stay under the fling cap at any density up to 4.
 * </ul>
 */
public class TouchApplication extends Application {
    private static final String TAG = "touch-test";

    private interface Section {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        guarded("down", this::down);
        guarded("move", this::move);
        guarded("two pointers", this::twoPointers);
        guarded("split", this::split);
        guarded("location", this::location);
        guarded("axes", this::axes);
        guarded("parcel", this::parcel);
        guarded("velocity", this::velocity);
        guarded("gesture", this::gesture);
        // TouchActivity ends the test with the touches the host plays (step 7b2).
        Log.i(TAG, "made-up events: done");
    }

    private void down() {
        MotionEvent down = MotionEvent.obtain(1000, 1000, MotionEvent.ACTION_DOWN, 10f, 20f, 0);
        Log.i(TAG, "down: action " + down.getAction() + ", at " + down.getX() + " " + down.getY() + ", raw "
                + down.getRawX() + " " + down.getRawY() + ", pointers " + down.getPointerCount() + ", id "
                + down.getPointerId(0) + ", tool " + down.getToolType(0) + ", source 0x"
                + Integer.toHexString(down.getSource()) + ", device " + down.getDeviceId() + ", times "
                + down.getDownTime() + " " + down.getEventTime() + ", pressure " + down.getPressure() + ", size "
                + down.getSize() + ", flags 0x" + Integer.toHexString(down.getFlags()) + ", edges "
                + down.getEdgeFlags() + ", precision " + down.getXPrecision() + " " + down.getYPrecision());
        down.recycle();
    }

    private void move() {
        MotionEvent move = moveWithHistory();
        StringBuilder history = new StringBuilder();
        for (int h = 0; h < move.getHistorySize(); h++) {
            history.append(" (").append(move.getHistoricalEventTime(h)).append(' ').append(move.getHistoricalX(h))
                    .append(' ').append(move.getHistoricalY(h)).append(')');
        }
        Log.i(TAG, "move: history " + move.getHistorySize() + history + ", now " + move.getEventTime() + " "
                + move.getX() + " " + move.getY() + ", pressure " + move.getPressure());
        move.recycle();
    }

    private static MotionEvent moveWithHistory() {
        MotionEvent move = MotionEvent.obtain(1000, 1008, MotionEvent.ACTION_MOVE, 30f, 40f, 0);
        move.addBatch(1016, 50f, 60f, 1f, 1f, 0);
        move.addBatch(1024, 70f, 80f, 0.5f, 1f, 0);
        return move;
    }

    private void twoPointers() {
        MotionEvent two = twoFingers(MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
                2010, 2);
        Log.i(TAG, "two pointers: action " + two.getActionMasked() + " index " + two.getActionIndex() + ", ids "
                + two.getPointerId(0) + " " + two.getPointerId(1) + ", second at " + two.getX(1) + " " + two.getY(1)
                + ", id 1 at index " + two.findPointerIndex(1) + ", tool " + two.getToolType(1));
        two.recycle();
    }

    /** A touch of one or two fingers at (50, 50) and (150, 50), from the touchscreen. */
    private static MotionEvent twoFingers(int action, long eventTime, int count) {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[count];
        for (int i = 0; i < count; i++) {
            properties[i] = new MotionEvent.PointerProperties();
            properties[i].id = i;
            properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = 50 + 100 * i;
            coords[i].y = 50;
            coords[i].pressure = 1;
            coords[i].size = 1;
        }
        return MotionEvent.obtain(2000, eventTime, action, count, properties, coords, 0, 0, 1f, 1f, 0, 0,
                InputDevice.SOURCE_TOUCHSCREEN, 0);
    }

    /** A row of two views, 100 px each: the first finger on the left one, the second on the right one. */
    private void split() {
        LinearLayout row = new LinearLayout(this);
        row.addView(new Target(this, "left"), new LinearLayout.LayoutParams(100, 100));
        row.addView(new Target(this, "right"), new LinearLayout.LayoutParams(100, 100));
        row.measure(View.MeasureSpec.makeMeasureSpec(200, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(100, View.MeasureSpec.EXACTLY));
        row.layout(0, 0, 200, 100);
        int second = 1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
        int[][] steps = {{MotionEvent.ACTION_DOWN, 1}, {MotionEvent.ACTION_POINTER_DOWN | second, 2},
                {MotionEvent.ACTION_MOVE, 2}, {MotionEvent.ACTION_POINTER_UP | second, 2}, {MotionEvent.ACTION_UP, 1}};
        for (int i = 0; i < steps.length; i++) {
            MotionEvent event = twoFingers(steps[i][0], 2000 + 10 * i, steps[i][1]);
            Log.i(TAG, "split: row handled " + MotionEvent.actionToString(event.getAction()) + ": "
                    + row.dispatchTouchEvent(event));
            event.recycle();
        }
    }

    private static final class Target extends View {
        private final String mName;

        Target(Application context, String name) {
            super(context);
            mName = name;
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            Log.i(TAG, "split: " + mName + " got " + MotionEvent.actionToString(event.getAction()) + " with "
                    + event.getPointerCount() + " pointer(s), id " + event.getPointerId(0) + " at " + event.getX()
                    + " " + event.getY());
            return true;
        }
    }

    private void location() {
        MotionEvent event = MotionEvent.obtain(3000, 3000, MotionEvent.ACTION_DOWN, 10f, 20f, 0);
        event.offsetLocation(5f, 7f);
        Log.i(TAG, "location: offset " + event.getX() + " " + event.getY() + ", raw " + event.getRawX() + " "
                + event.getRawY());
        Matrix matrix = new Matrix();
        matrix.setScale(2f, 3f);
        event.transform(matrix);
        Log.i(TAG, "location: scaled " + event.getX() + " " + event.getY() + ", raw " + event.getRawX() + " "
                + event.getRawY());
        event.setLocation(1f, 2f);
        Log.i(TAG, "location: set " + event.getX() + " " + event.getY() + ", raw " + event.getRawX() + " "
                + event.getRawY());
        event.recycle();
    }

    private void axes() {
        Log.i(TAG, "axes: " + MotionEvent.axisToString(MotionEvent.AXIS_X) + " "
                + MotionEvent.axisToString(MotionEvent.AXIS_PRESSURE) + " "
                + MotionEvent.axisToString(MotionEvent.AXIS_GESTURE_PINCH_SCALE_FACTOR) + " "
                + MotionEvent.axisToString(99) + ", from labels " + MotionEvent.axisFromString("AXIS_Y") + " "
                + MotionEvent.axisFromString("AXIS_RELATIVE_X") + " " + MotionEvent.axisFromString("17") + " "
                + MotionEvent.axisFromString("AXIS_NONE"));
    }

    private void parcel() {
        MotionEvent move = moveWithHistory();
        Parcel parcel = Parcel.obtain();
        move.writeToParcel(parcel, 0);
        int size = parcel.dataSize();
        parcel.setDataPosition(0);
        MotionEvent back = MotionEvent.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        Log.i(TAG, "parcel: " + size + " bytes; back: action " + back.getAction() + ", history "
                + back.getHistorySize() + ", at " + back.getX() + " " + back.getY() + ", first sample "
                + back.getHistoricalX(0) + " at " + back.getHistoricalEventTime(0) + ", times " + back.getDownTime()
                + " " + back.getEventTime() + ", pressure " + back.getPressure());
        move.recycle();
        back.recycle();
    }

    /** A swipe to the right and down: a DOWN, a MOVE every 8 ms, faster each time, and the UP 8 ms after. */
    private static MotionEvent[] swipe() {
        MotionEvent[] events = new MotionEvent[10];
        events[0] = MotionEvent.obtain(5000, 5000, MotionEvent.ACTION_DOWN, 0f, 0f, 0);
        for (int i = 1; i <= 8; i++) {
            events[i] = MotionEvent.obtain(5000, 5000 + 8 * i, MotionEvent.ACTION_MOVE, 40f * i + 0.5f * i * i,
                    15f * i, 0);
        }
        events[9] = MotionEvent.obtain(5000, 5072, MotionEvent.ACTION_UP, 352f, 120f, 0);
        return events;
    }

    private void velocity() {
        VelocityTracker tracker = VelocityTracker.obtain();
        for (MotionEvent event : swipe()) {
            tracker.addMovement(event);
            event.recycle();
        }
        tracker.computeCurrentVelocity(1000);
        String free = rounded(tracker.getXVelocity()) + " " + rounded(tracker.getYVelocity());
        tracker.computeCurrentVelocity(1000, 3000f);
        Log.i(TAG, "velocity: " + free + " px/s, capped at 3000: " + rounded(tracker.getXVelocity()) + " "
                + rounded(tracker.getYVelocity()) + ", axis X " + rounded(tracker.getAxisVelocity(MotionEvent.AXIS_X))
                + ", supports X " + tracker.isAxisSupported(MotionEvent.AXIS_X) + ", scroll "
                + tracker.isAxisSupported(MotionEvent.AXIS_SCROLL) + ", pressure "
                + tracker.isAxisSupported(MotionEvent.AXIS_PRESSURE));
        tracker.recycle();
    }

    private void gesture() {
        int[] scrolls = {0};
        GestureDetector detector = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDown(MotionEvent e) {
                Log.i(TAG, "gesture: down");
                return true;
            }

            @Override
            public boolean onScroll(MotionEvent e1, MotionEvent e2, float distanceX, float distanceY) {
                scrolls[0]++;
                return true;
            }

            @Override
            public boolean onFling(MotionEvent e1, MotionEvent e2, float velocityX, float velocityY) {
                Log.i(TAG, "gesture: fling " + rounded(velocityX) + " " + rounded(velocityY) + " px/s");
                return true;
            }
        });
        for (MotionEvent event : swipe()) {
            detector.onTouchEvent(event);
            event.recycle();
        }
        Log.i(TAG, "gesture: scrolls " + scrolls[0]);
    }

    private static String rounded(float value) {
        return String.format(Locale.ROOT, "%.1f", value);
    }

    /** Runs a section; what it throws is logged with its name. */
    private static void guarded(String name, Section section) {
        try {
            section.run();
        } catch (Throwable e) {
            Log.i(TAG, name + ": " + e);
        }
    }
}
