package io.github.kisame76.minato.test.key;

import android.app.Application;
import android.os.Parcel;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;

/**
 * ADR-0045 step 7c1's test of the virtual keyboard's key character map, which Android's EventHub loads from
 * Virtual.kcm: what keys type alone and with modifiers, a dead key, numbers, labels and matches against the digits that
 * DigitsKeyListener accepts, printing keys, the events that type a string, a parcel round trip, and what a made-up
 * KeyEvent asks of it. Each line is the same on any device; the devices' maps go under the device tag.
 */
public class KeyApplication extends Application {
    private static final String TAG = "key-test";
    private static final String DEVICE_TAG = "key-test-device";
    private static final char[] DIGITS = "0123456789".toCharArray();

    @Override
    public void onCreate() {
        super.onCreate();
        KeyCharacterMap map = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
        Log.i(TAG, "virtual: keyboard type " + map.getKeyboardType() + ", modifier behavior "
                + map.getModifierBehavior());
        Log.i(TAG, "get: A " + hex(map.get(KeyEvent.KEYCODE_A, 0)) + ", Shift+A "
                + hex(map.get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON)) + ", Caps Lock+A "
                + hex(map.get(KeyEvent.KEYCODE_A, KeyEvent.META_CAPS_LOCK_ON)) + ", Shift+Caps Lock+A "
                + hex(map.get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON | KeyEvent.META_CAPS_LOCK_ON)) + ", 1 "
                + hex(map.get(KeyEvent.KEYCODE_1, 0)) + ", Shift+1 "
                + hex(map.get(KeyEvent.KEYCODE_1, KeyEvent.META_SHIFT_ON)) + ", SPACE "
                + hex(map.get(KeyEvent.KEYCODE_SPACE, 0)) + ", DEL " + hex(map.get(KeyEvent.KEYCODE_DEL, 0)));
        int accent = map.get(KeyEvent.KEYCODE_E, KeyEvent.META_ALT_ON);
        Log.i(TAG, "dead: Alt+E " + hex(accent) + ", on e "
                + hex(KeyCharacterMap.getDeadChar(accent & KeyCharacterMap.COMBINING_ACCENT_MASK, 'e')));
        Log.i(TAG, "number: A " + hex(map.getNumber(KeyEvent.KEYCODE_A)) + ", 1 "
                + hex(map.getNumber(KeyEvent.KEYCODE_1)) + "; label: A " + hex(map.getDisplayLabel(KeyEvent.KEYCODE_A))
                + ", 1 " + hex(map.getDisplayLabel(KeyEvent.KEYCODE_1)));
        Log.i(TAG, "match: 1 in the digits " + hex(map.getMatch(KeyEvent.KEYCODE_1, DIGITS)) + ", A in the digits "
                + hex(map.getMatch(KeyEvent.KEYCODE_A, DIGITS)) + ", 1 in ! "
                + hex(map.getMatch(KeyEvent.KEYCODE_1, new char[] {'!'})));
        Log.i(TAG, "printing: A " + map.isPrintingKey(KeyEvent.KEYCODE_A) + ", SPACE "
                + map.isPrintingKey(KeyEvent.KEYCODE_SPACE) + ", DEL " + map.isPrintingKey(KeyEvent.KEYCODE_DEL));

        KeyEvent[] events = map.getEvents("Hi!".toCharArray());
        StringBuilder line = new StringBuilder("events of Hi!: " + events.length + ",");
        for (KeyEvent event : events) {
            line.append(event.getAction() == KeyEvent.ACTION_DOWN ? " down " : " up ")
                    .append(KeyEvent.keyCodeToString(event.getKeyCode()));
            if (event.getMetaState() != 0) {
                line.append('/').append(hex(event.getMetaState()));
            }
        }
        Log.i(TAG, line + "; device " + events[0].getDeviceId() + ", source " + hex(events[0].getSource()));

        Parcel parcel = Parcel.obtain();
        map.writeToParcel(parcel, 0);
        int size = parcel.dataSize();
        parcel.setDataPosition(0);
        KeyCharacterMap copy = KeyCharacterMap.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        Log.i(TAG, "parcel: " + size + " bytes, equal " + copy.equals(map) + ", keyboard type "
                + copy.getKeyboardType() + ", Shift+1 " + hex(copy.get(KeyEvent.KEYCODE_1, KeyEvent.META_SHIFT_ON)));

        KeyEvent one = new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_1);
        Log.i(TAG, "event: device " + one.getDeviceId() + ", unicode " + hex(one.getUnicodeChar()) + ", with Shift "
                + hex(one.getUnicodeChar(KeyEvent.META_SHIFT_ON)) + ", number " + hex(one.getNumber()) + ", match "
                + hex(one.getMatch(DIGITS)) + ", label " + hex(one.getDisplayLabel()) + ", printing "
                + one.isPrintingKey() + ", map's keyboard type " + one.getKeyCharacterMap().getKeyboardType());

        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            KeyCharacterMap deviceMap = device.getKeyCharacterMap();
            Log.i(DEVICE_TAG, "device " + id + " " + device.getName() + ": sources " + hex(device.getSources())
                    + ", keyboard type " + device.getKeyboardType() + ", map's keyboard type "
                    + deviceMap.getKeyboardType() + ", A " + hex(deviceMap.get(KeyEvent.KEYCODE_A, 0)));
        }
    }

    private static String hex(int value) {
        return "0x" + Integer.toHexString(value);
    }
}
