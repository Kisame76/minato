package io.github.kisame76.minato.test.keyboard;

import android.app.Activity;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.widget.EditText;

/**
 * ADR-0068 step 7c2's test of a hardware keyboard: the host adds a keyboard to the session's input devices
 * (apk-run's device:add), types "h", "i" and DEL on it (hwkey: steps) and removes it (device:remove). The lines
 * name what InputManager tells the app of the device (added, its sources, keyboard type, map and the map's fallback
 * for BUTTON_X, which Generic.kcm gives and Virtual.kcm does not), what the EditText holds after each key, and the
 * removal. Minato's lines only: an Android device has its own keyboards, with their own maps.
 */
public class KeyboardActivity extends Activity implements InputManager.InputDeviceListener {
    private static final String TAG = "keyboard-test";

    private EditText mEdit;
    private int mUps;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mEdit = new EditText(this);
        mEdit.setOnKeyListener((view, keyCode, event) -> {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                int up = ++mUps;
                mEdit.post(() -> Log.i(TAG, "key " + up + " " + KeyEvent.keyCodeToString(keyCode) + ": text \""
                        + mEdit.getText() + "\", device " + event.getDeviceId() + " "
                        + (event.getDevice() != null ? "known" : "unknown") + ", source 0x"
                        + Integer.toHexString(event.getSource())));
            }
            return false;
        });
        setContentView(mEdit);
        mEdit.requestFocus();
        InputManager manager = getSystemService(InputManager.class);
        manager.registerInputDeviceListener(this, null);
        Log.i(TAG, "ready: devices " + describeIds());
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        InputDevice device = InputDevice.getDevice(deviceId);
        KeyCharacterMap map = device.getKeyCharacterMap();
        KeyCharacterMap.FallbackAction x = map.getFallbackAction(KeyEvent.KEYCODE_BUTTON_X, 0);
        Log.i(TAG, "added: " + device.getName() + ", sources 0x" + Integer.toHexString(device.getSources())
                + ", keyboard type " + device.getKeyboardType() + ", external " + device.isExternal()
                + ", map's keyboard type " + map.getKeyboardType() + ", A 0x" + Integer.toHexString(map.get(
                        KeyEvent.KEYCODE_A, 0)) + ", Shift+A 0x" + Integer.toHexString(
                        map.get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON)) + ", BUTTON_X falls back to "
                + (x == null ? "nothing" : KeyEvent.keyCodeToString(x.keyCode)));
        Log.i(TAG, "devices: " + describeIds());
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        Log.i(TAG, "changed");
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        Log.i(TAG, "removed: known " + (InputDevice.getDevice(deviceId) != null) + ", devices " + describeIds());
        Log.i(TAG, "done");
    }

    private static String describeIds() {
        StringBuilder ids = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            ids.append(ids.length() == 0 ? "" : " ").append(id);
        }
        return ids.toString();
    }
}
