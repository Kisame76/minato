package io.github.kisame76.minato.test.mouse;

import android.app.Activity;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;

import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0068 decision 5, the mouse step: the window logs "ready: mouse" and then what a mouse that appears does. On
 * Android 15's emulator it is a virtual device of `uinput` (fixtures/apps/mouse-test/uinput.json, which
 * tools/android-reference.sh starts at the ready line); in Minato the host's GCMouse or UIPointer. The lines are the
 * device that InputManager reports (name, sources, ranges by axis), and for each motion event of a mouse the callback
 * that got it (hover, touch or generic), its action, source, tool, buttons, scroll axes and whether the position or
 * the relative axes moved. Times and positions are left out; the order is the mouse's. "done" follows the removal.
 */
public class MouseActivity extends Activity {
    private static final String TAG = "mouse-test";

    private int mMouseId = -1;
    private boolean mStarted;

    private static boolean isMouse(InputDevice device) {
        return (device.getSources() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE;
    }

    private static String toolName(int tool) {
        final String[] names = {"UNKNOWN", "FINGER", "STYLUS", "MOUSE", "ERASER", "PALM"};
        return tool >= 0 && tool < names.length ? names[tool] : Integer.toString(tool);
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /** The view that logs what it is given, hover first and then generic: onHoverEvent declines, as a plain view's. */
    private static final class MouseView extends View {
        private float mLastX = Float.NaN;
        private float mLastY = Float.NaN;
        private long mLastTime = -1;
        private boolean mMoved;

        MouseView(Context context) {
            super(context);
        }

        @Override
        public boolean onHoverEvent(MotionEvent event) {
            if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
                logMotion("hover", event);
            }
            return false;
        }

        @Override
        public boolean onGenericMotionEvent(MotionEvent event) {
            if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
                logMotion("generic", event);
                return true;
            }
            return false;
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
                logMotion("touch", event);
                return true;
            }
            return false;
        }

        private void logMotion(String callback, MotionEvent event) {
            // The hover callback and the generic one get the same event: it moved against the one before it.
            if (event.getEventTime() != mLastTime) {
                mMoved = event.getX() != mLastX || event.getY() != mLastY;
                mLastX = event.getX();
                mLastY = event.getY();
                mLastTime = event.getEventTime();
            }
            final boolean moved = mMoved;
            final StringBuilder line = new StringBuilder(callback + " " + MotionEvent.actionToString(event.getAction())
                    + ", source 0x" + Integer.toHexString(event.getSource()) + ", tool "
                    + toolName(event.getToolType(0)) + ", pointers " + event.getPointerCount()
                    + ", history " + event.getHistorySize() + ", buttons 0x"
                    + Integer.toHexString(event.getButtonState()) + ", action button 0x"
                    + Integer.toHexString(event.getActionButton()) + ", meta 0x"
                    + Integer.toHexString(event.getMetaState()) + ", moved " + moved);
            final float vscroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
            final float hscroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
            if (vscroll != 0f) {
                line.append(", VSCROLL=").append(vscroll);
            }
            if (hscroll != 0f) {
                line.append(", HSCROLL=").append(hscroll);
            }
            if (event.getAxisValue(MotionEvent.AXIS_RELATIVE_X) != 0f
                    || event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y) != 0f) {
                line.append(", relative");
            }
            log(line.toString());
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final FrameLayout root = new FrameLayout(this);
        root.addView(new MouseView(this), new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        final InputManager manager = getSystemService(InputManager.class);
        manager.registerInputDeviceListener(new InputManager.InputDeviceListener() {
            @Override
            public void onInputDeviceAdded(int deviceId) {
                final InputDevice device = InputDevice.getDevice(deviceId);
                if (device != null && isMouse(device) && mMouseId < 0 && device.getName().startsWith("Minato")) {
                    mMouseId = deviceId;
                    logDevice(device);
                }
            }

            @Override
            public void onInputDeviceRemoved(int deviceId) {
                if (deviceId == mMouseId) {
                    log("removed: the mouse is no longer listed: " + (InputDevice.getDevice(deviceId) == null));
                    mMouseId = -1;
                    log("done");
                }
            }

            @Override
            public void onInputDeviceChanged(int deviceId) {
            }
        }, null);
    }

    /** The keys a mouse makes (its back and forward buttons) are logged and taken, so that BACK does not end the
        window. */
    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent event) {
        final InputDevice device = event.getDevice();
        if (device != null && isMouse(device)) {
            log("key " + (event.getAction() == android.view.KeyEvent.ACTION_DOWN ? "down " : "up ")
                    + android.view.KeyEvent.keyCodeToString(event.getKeyCode()) + ", source 0x"
                    + Integer.toHexString(event.getSource()));
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            log("ready: mouse");
        }
    }

    private void logDevice(InputDevice device) {
        log("added: name \"" + device.getName() + "\", sources 0x" + Integer.toHexString(device.getSources())
                + ", vendor 0x" + Integer.toHexString(device.getVendorId()) + ", product 0x"
                + Integer.toHexString(device.getProductId()) + ", virtual " + device.isVirtual()
                + ", external " + device.isExternal() + ", keyboard type " + device.getKeyboardType());
        final List<String> ranges = new ArrayList<>();
        for (InputDevice.MotionRange range : device.getMotionRanges()) {
            ranges.add(MotionEvent.axisToString(range.getAxis()) + " source 0x" + Integer.toHexString(range.getSource())
                    + " flat " + range.getFlat() + " fuzz " + range.getFuzz());
        }
        ranges.sort(null);
        for (String line : ranges) {
            log("range " + line);
        }
    }
}
