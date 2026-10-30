package io.github.kisame76.minato.test.gamepad;

import android.app.Activity;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;

import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0068 decision 5, the controllers' step 1: the window logs "ready: pad" and then what a game controller that
 * appears does. On Android 15's emulator it is a virtual device of `uinput` (fixtures/apps/gamepad-test/uinput.json,
 * which tools/android-reference.sh starts at the ready line); in Minato the host's GCController. The lines are the
 * device that InputManager reports (name, sources, vendor and product, and every motion range by axis), each key the
 * focused view gets (action, key code, source) and each generic motion event of the joystick source (action,
 * source, the axes that are not zero), and the device's going. Timing and ID values are left out; the order is the
 * pad's. "done" follows the removal.
 */
public class GamepadActivity extends Activity {
    private static final String TAG = "gamepad-test";
    private static final int[] AXES = {
        MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RX, MotionEvent.AXIS_RY,
        MotionEvent.AXIS_RZ, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_BRAKE,
        MotionEvent.AXIS_GAS, MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y,
    };

    private int mPadId = -1;
    private boolean mStarted;

    /**
     * A game controller: SOURCE_GAMEPAD is a class bit and the keyboard source, which every keyboard has, shares
     * it.
     */
    private static boolean isPad(InputDevice device) {
        return (device.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final FrameLayout root = new FrameLayout(this) {
            @Override
            public boolean dispatchGenericMotionEvent(MotionEvent event) {
                if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                    logMotion(event);
                    return true;
                }
                return super.dispatchGenericMotionEvent(event);
            }

            @Override
            public boolean dispatchKeyEvent(KeyEvent event) {
                final InputDevice device = event.getDevice();
                if (device != null && isPad(device)) {
                    if (event.getRepeatCount() > 0) {
                        return true;  // a held key's repeats are the device's timing, not its report
                    }
                    log("key " + (event.getAction() == KeyEvent.ACTION_DOWN ? "down " : "up ")
                            + KeyEvent.keyCodeToString(event.getKeyCode()) + ", source 0x"
                            + Integer.toHexString(event.getSource()));
                    return true;
                }
                return super.dispatchKeyEvent(event);
            }
        };
        root.setFocusable(true);
        root.setFocusableInTouchMode(true);
        setContentView(root);
        root.requestFocus();
        final InputManager manager = getSystemService(InputManager.class);
        manager.registerInputDeviceListener(new InputManager.InputDeviceListener() {
            @Override
            public void onInputDeviceAdded(int deviceId) {
                final InputDevice device = InputDevice.getDevice(deviceId);
                if (device != null && isPad(device) && mPadId < 0) {
                    mPadId = deviceId;
                    logDevice(device);
                }
            }

            @Override
            public void onInputDeviceRemoved(int deviceId) {
                if (deviceId == mPadId) {
                    log("removed: the pad is no longer listed: " + (InputDevice.getDevice(deviceId) == null));
                    mPadId = -1;
                    log("done");
                }
            }

            @Override
            public void onInputDeviceChanged(int deviceId) {
            }
        }, null);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mStarted) {
            mStarted = true;
            int pads = 0;
            for (int id : InputDevice.getDeviceIds()) {
                final InputDevice device = InputDevice.getDevice(id);
                if (device != null && isPad(device)) {
                    pads++;
                }
            }
            log("pads before: " + pads);
            log("ready: pad");
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
                    + " [" + range.getMin() + ", " + range.getMax() + "] flat " + range.getFlat() + " fuzz "
                    + range.getFuzz());
        }
        ranges.sort(null);
        for (String line : ranges) {
            log("range " + line);
        }
        final int[] codes = {KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_THUMBL,
            KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_CENTER};
        final boolean[] has = device.hasKeys(codes);
        final StringBuilder keys = new StringBuilder("keys:");
        for (int i = 0; i < codes.length; i++) {
            if (has[i]) {
                keys.append(' ').append(KeyEvent.keyCodeToString(codes[i]).substring("KEYCODE_".length()));
            }
        }
        log(keys.toString());
    }

    private void logMotion(MotionEvent event) {
        final StringBuilder line = new StringBuilder("motion " + MotionEvent.actionToString(event.getAction())
                + ", source 0x" + Integer.toHexString(event.getSource()) + ", pointers " + event.getPointerCount()
                + ", history " + event.getHistorySize() + ", axes");
        for (int axis : AXES) {
            final float value = event.getAxisValue(axis);
            if (value != 0f) {
                line.append(' ').append(MotionEvent.axisToString(axis)).append('=').append(value);
            }
        }
        log(line.toString());
    }
}
