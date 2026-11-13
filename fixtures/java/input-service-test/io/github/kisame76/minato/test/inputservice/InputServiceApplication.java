package io.github.kisame76.minato.test.inputservice;

import android.app.Application;
import android.content.Context;
import android.hardware.input.InputManager;
import android.util.Log;
import android.view.GestureDetector;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.ScaleGestureDetector;

import java.util.List;

/**
 * ADR-0045's test of the input service that Godot's start asks for, in
 * Application.onCreate, before its launcher activity starts:
 * <ul>
 *   <li>the input service's class, and a device listener registered and unregistered, as Godot's input handler
 *       registers one;
 *   <li>every device id, each device asked for as Godot's initInputDevices does: the virtual keyboard, which every
 *       Android device has alike, with its name, descriptor, sources and keyboard type; any other device, which
 *       differs by device, under the device tag with its sources and motion ranges;
 *   <li>the virtual keyboard once more through InputDevice.getDevice;
 *   <li>a GestureDetector and a ScaleGestureDetector made and set as Godot sets them.
 * </ul>
 */
public class InputServiceApplication extends Application {
    private static final String TAG = "input-service-test";
    private static final String DEVICE = TAG + "-device";

    private interface Section {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        guarded("input service", this::inputService);
        guarded("gesture detectors", this::detectors);
        Log.i(TAG, "done");
    }

    private void inputService() {
        InputManager input = (InputManager) getSystemService(Context.INPUT_SERVICE);
        Log.i(TAG, "input service: " + input.getClass().getName());
        InputManager.InputDeviceListener listener = new InputManager.InputDeviceListener() {
            @Override
            public void onInputDeviceAdded(int deviceId) {
            }

            @Override
            public void onInputDeviceRemoved(int deviceId) {
            }

            @Override
            public void onInputDeviceChanged(int deviceId) {
            }
        };
        input.registerInputDeviceListener(listener, null);
        input.unregisterInputDeviceListener(listener);
        Log.i(TAG, "device listener registered and unregistered");
        boolean virtualKeyboard = false;
        for (int id : input.getInputDeviceIds()) {
            InputDevice device = input.getInputDevice(id);
            if (id == KeyCharacterMap.VIRTUAL_KEYBOARD) {
                virtualKeyboard = true;
                Log.i(TAG, "virtual keyboard: " + describe(device));
            } else {
                Log.i(DEVICE, "device " + id + ": " + describe(device) + ", ranges " + ranges(device));
            }
        }
        Log.i(TAG, "device ids hold the virtual keyboard: " + virtualKeyboard);
        InputDevice again = InputDevice.getDevice(KeyCharacterMap.VIRTUAL_KEYBOARD);
        Log.i(TAG, "InputDevice.getDevice(VIRTUAL_KEYBOARD): " + (again == null ? "null" : describe(again)));
    }

    private static String describe(InputDevice device) {
        return "name " + device.getName() + ", descriptor " + device.getDescriptor() + ", sources 0x"
                + Integer.toHexString(device.getSources()) + ", keyboard type " + device.getKeyboardType()
                + ", virtual " + device.isVirtual() + ", external " + device.isExternal() + ", vendor "
                + device.getVendorId() + ", product " + device.getProductId() + ", motion ranges "
                + device.getMotionRanges().size();
    }

    private static String ranges(InputDevice device) {
        StringBuilder text = new StringBuilder();
        List<InputDevice.MotionRange> ranges = device.getMotionRanges();
        for (InputDevice.MotionRange range : ranges) {
            text.append(text.length() == 0 ? "" : ", ").append(range.getAxis()).append(' ').append(range.getMin())
                    .append("..").append(range.getMax());
        }
        return "[" + text + "]";
    }

    private void detectors() {
        GestureDetector gestures = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener());
        gestures.setIsLongpressEnabled(false);
        Log.i(TAG, "GestureDetector: long press " + gestures.isLongpressEnabled());
        ScaleGestureDetector scale = new ScaleGestureDetector(this,
                new ScaleGestureDetector.SimpleOnScaleGestureListener());
        Log.i(TAG, "ScaleGestureDetector: quick scale " + scale.isQuickScaleEnabled() + ", stylus scale "
                + scale.isStylusScaleEnabled());
        scale.setStylusScaleEnabled(true);
        Log.i(TAG, "ScaleGestureDetector after setStylusScaleEnabled(true): " + scale.isStylusScaleEnabled());
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
