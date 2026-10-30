/*
 * The input service (ADR-0045 decisions 2, 3 and 6; gap 7a of the Godot survey): the IInputManager of
 * InputManagerService, local in the app's process, which ServiceManager's table gives as "input". It is AIDL's Default
 * class, as the display service is. Its answers:
 *   - getInputDeviceIds and getInputDevice: Minato's devices, built with InputDevice.Builder.
 *     - The virtual keyboard, id -1, as Android's EventHub makes it (createVirtualKeyboardLocked): "Virtual", a
 *       keyboard with letters and a d-pad, with Virtual.kcm's key character map as EventHub loads it (step 7c1).
 *     - The touchscreen, id 1, M3's input device (runtime/android/input.cpp:17): a finger's coordinates on the
 *       session's screen, its pressure and its touch and tool sizes, the axes M3's events carry. It has no keys, so
 *       its key character map is empty, as Android's are for such devices (step 7c1).
 *     - A hardware keyboard, when the host says it is there (ADR-0068 step 7c2): the host's device record becomes an
 *       alphabetic external keyboard with Generic.kcm's map, the one EventHub gives a keyboard that has no map of its
 *       own, and the listeners are told, as InputReader's device list change tells them.
 *   - registerInputDevicesChangedListener keeps the listener, which hears of the host's keyboards coming and going.
 *   - The 10 methods that IInputManager.aidl guards with @EnforcePermission check the app's permission and throw
 *     SecurityException with PermissionEnforcer's message without it. Each but BLUETOOTH is a signature permission
 *     (decision 6); getInputDeviceBluetoothAddress then answers null, as for a device that is not a Bluetooth one.
 * The rest returns AIDL's defaults, a listed gap. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles without android.*.
 */
package android.hardware.input;

import android.Manifest;
import android.app.ActivityThread;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.res.Resources;
import android.os.Binder;
import android.os.IBinder;
import android.util.DisplayMetrics;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.KeyCharacterMap;
import android.view.MotionEvent;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;

import android.os.RemoteException;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public final class InputService extends IInputManager.Default {
    /** PermissionEnforcer's message. */
    private static final String ACCESS_DENIED = "Access denied, requires: ";
    /** M3's input device (runtime/android/input.cpp:17). */
    private static final int TOUCHSCREEN_ID = 1;
    /** EventHub's descriptor of the virtual keyboard: its identifier's uniqueId, "<virtual>", hashed. */
    private static final String VIRTUAL_KEYBOARD_DESCRIPTOR = "a718a782d34bc767f4689c232d64d527998ea7fd";

    private static final String TAG = "InputService";
    /** The host's devices (ADR-0068 step 7c2), by the host's ID; their changes are told to the instance, if any. */
    private static final List<InputDevice> sHostDevices = new ArrayList<>();
    private static InputService sInstance;
    private static int sGeneration;

    private final Binder mBinder = new Binder();
    private final List<IInputDevicesChangedListener> mListeners = new ArrayList<>();
    private InputDevice[] mDevices;

    private InputService() {
        mBinder.attachInterface(this, IInputManager.Stub.DESCRIPTOR);
        synchronized (sHostDevices) {
            sInstance = this;
        }
    }

    /**
     * A device record of the host (ADR-0068 step 7c2), on the main thread, from the input channel's receiver: a
     * keyboard comes (op 0), goes (1) or changes (2).
     */
    public static void onHostDevice(int op, int id, int sources, int vendor, int product, String name) {
        InputService service;
        synchronized (sHostDevices) {
            InputDevice known = null;
            for (int i = sHostDevices.size() - 1; i >= 0; i--) {
                if (sHostDevices.get(i).getId() == id) {
                    known = sHostDevices.remove(i);
                }
            }
            // A repeat of a record, which every window's channel may bring, changes nothing; so does the removal of
            // a device that is not there.
            if (op == 1 ? known == null : known != null && known.getName().equals(name)
                    && known.getSources() == sources && known.getVendorId() == vendor
                    && known.getProductId() == product) {
                if (known != null) {
                    sHostDevices.add(known);
                }
                return;
            }
            if (op != 1) {
                final boolean pad = isPad(sources);
                final InputDevice.Builder device = new InputDevice.Builder()
                        .setId(id)
                        .setName(name)
                        .setDescriptor("minato-host-" + id)
                        .setSources(sources)
                        .setVendorId(vendor)
                        .setProductId(product)
                        .setGeneration(++sGeneration)
                        .setExternal(true)
                        .setKeyboardType(pad ? InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC
                                : InputDevice.KEYBOARD_TYPE_ALPHABETIC)
                        .setKeyCharacterMap(nativeKeyboardKeyCharacterMap(id));
                if (pad) {
                    addPadRanges(device);
                }
                sHostDevices.add(device.build());
            }
            service = sInstance;
        }
        if (service != null) {
            service.devicesChanged();
        }
    }

    /** A game controller: SOURCE_GAMEPAD is a class bit that the keyboard source shares, so all its bits are asked. */
    private static boolean isPad(int sources) {
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
    }

    /**
     * The motion ranges of the standard pad, as Android 15 reports them for an Xbox Wireless Controller (gamepad-test):
     * the sticks, HAT_X and HAT_Y from -1 to 1, the triggers from 0 to 1, each trigger as two axes.
     */
    private static void addPadRanges(InputDevice.Builder device) {
        final int source = InputDevice.SOURCE_JOYSTICK;
        for (int axis : new int[] {MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
                MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y}) {
            device.addMotionRange(axis, source, -1, 1, 0, 0, 0);
        }
        for (int axis : new int[] {MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_BRAKE,
                MotionEvent.AXIS_GAS}) {
            device.addMotionRange(axis, source, 0, 1, 0, 0, 0);
        }
    }

    /**
     * The buttons the standard pad has (gamepad-test's hasKeys): A, B, X, Y, L1, R1, both sticks, START, SELECT, MODE.
     */
    private static final int[] PAD_KEYS = {KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
        KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_START,
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_MODE};

    /** As InputManagerService's: which of the key codes the device has; a game controller has its buttons. */
    @Override
    public boolean hasKeys(int deviceId, int sourceMask, int[] keyCodes, boolean[] keyExists) {
        final InputDevice device = getInputDevice(deviceId);
        if (device == null || (device.getSources() & sourceMask) == 0 || !isPad(device.getSources())) {
            return false;
        }
        for (int at = 0; at < keyCodes.length; at++) {
            keyExists[at] = false;
            for (int code : PAD_KEYS) {
                keyExists[at] |= code == keyCodes[at];
            }
        }
        return true;
    }

    /** InputReader's device list change: every device's ID and generation, to each listener. */
    private void devicesChanged() {
        InputDevice[] devices = devices();
        int[] idAndGeneration = new int[devices.length * 2];
        for (int i = 0; i < devices.length; i++) {
            idAndGeneration[2 * i] = devices[i].getId();
            idAndGeneration[2 * i + 1] = devices[i].getGeneration();
        }
        List<IInputDevicesChangedListener> listeners;
        synchronized (mListeners) {
            listeners = new ArrayList<>(mListeners);
        }
        for (IInputDevicesChangedListener listener : listeners) {
            try {
                listener.onInputDevicesChanged(idAndGeneration);
            } catch (RemoteException e) {
                Log.w(TAG, "A listener of the input devices is gone", e);
            }
        }
    }

    /** ServiceManager's maker for "input": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new InputService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public int[] getInputDeviceIds() {
        InputDevice[] devices = devices();
        int[] ids = new int[devices.length];
        for (int i = 0; i < devices.length; i++) {
            ids[i] = devices[i].getId();
        }
        return ids;
    }

    @Override
    public InputDevice getInputDevice(int deviceId) {
        for (InputDevice device : devices()) {
            if (device.getId() == deviceId) {
                return device;
            }
        }
        return null;
    }

    @Override
    public void registerInputDevicesChangedListener(IInputDevicesChangedListener listener) {
        synchronized (mListeners) {
            mListeners.add(listener);
        }
    }

    @Override
    public void setKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier, int userId,
            InputMethodInfo imeInfo, InputMethodSubtype imeSubtype, String keyboardLayoutDescriptor) {
        enforce(Manifest.permission.SET_KEYBOARD_LAYOUT);
    }

    @Override
    public void remapModifierKey(int fromKey, int toKey) {
        enforce(Manifest.permission.REMAP_MODIFIER_KEYS);
    }

    @Override
    public void clearAllModifierKeyRemappings() {
        enforce(Manifest.permission.REMAP_MODIFIER_KEYS);
    }

    @Override
    @SuppressWarnings("rawtypes") // AIDL's Map.
    public Map getModifierKeyRemapping() {
        enforce(Manifest.permission.REMAP_MODIFIER_KEYS);
        return null;
    }

    @Override
    public String getInputDeviceBluetoothAddress(int deviceId) {
        enforce(Manifest.permission.BLUETOOTH);
        return null;
    }

    @Override
    public void pilferPointers(IBinder inputChannelToken) {
        enforce(Manifest.permission.MONITOR_INPUT);
    }

    @Override
    public void registerKeyboardBacklightListener(IKeyboardBacklightListener listener) {
        enforce(Manifest.permission.MONITOR_KEYBOARD_BACKLIGHT);
    }

    @Override
    public void unregisterKeyboardBacklightListener(IKeyboardBacklightListener listener) {
        enforce(Manifest.permission.MONITOR_KEYBOARD_BACKLIGHT);
    }

    @Override
    public void registerStickyModifierStateListener(IStickyModifierStateListener listener) {
        enforce(Manifest.permission.MONITOR_STICKY_MODIFIER_STATE);
    }

    @Override
    public void unregisterStickyModifierStateListener(IStickyModifierStateListener listener) {
        enforce(Manifest.permission.MONITOR_STICKY_MODIFIER_STATE);
    }

    /** As PermissionEnforcer.enforcePermission, with the app's own permissions (decision 6). */
    private static void enforce(String permission) {
        Context context = ActivityThread.currentApplication();
        if (context == null || context.checkCallingOrSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException(ACCESS_DENIED + permission);
        }
    }

    /** The devices, made at the first question, when the session's screen is in the system Resources. */
    private synchronized InputDevice[] devices() {
        if (mDevices == null) {
            mDevices = new InputDevice[] {virtualKeyboard(), touchscreen()};
        }
        synchronized (sHostDevices) {
            InputDevice[] all = new InputDevice[mDevices.length + sHostDevices.size()];
            System.arraycopy(mDevices, 0, all, 0, mDevices.length);
            for (int i = 0; i < sHostDevices.size(); i++) {
                all[mDevices.length + i] = sHostDevices.get(i);
            }
            return all;
        }
    }

    /** EventHub's virtual keyboard: KEYBOARD, ALPHAKEY, DPAD and VIRTUAL, which InputReader reports as below. */
    private static InputDevice virtualKeyboard() {
        return new InputDevice.Builder()
                .setId(KeyCharacterMap.VIRTUAL_KEYBOARD)
                .setName("Virtual")
                .setDescriptor(VIRTUAL_KEYBOARD_DESCRIPTOR)
                .setSources(InputDevice.SOURCE_KEYBOARD | InputDevice.SOURCE_DPAD)
                .setKeyboardType(InputDevice.KEYBOARD_TYPE_ALPHABETIC)
                .setKeyCharacterMap(nativeVirtualKeyCharacterMap())
                .build();
    }

    /**
     * M3's touchscreen on the session's screen, the display service's: the axes its events carry
     * (runtime/android/input.cpp:39-49), in pixels from 0 to the last one, and the touch and tool sizes up to the
     * screen's diagonal, as Android's touchscreens report them. Without a session the screen is 0 x 0, and so are they.
     */
    private static InputDevice touchscreen() {
        DisplayMetrics metrics = Resources.getSystem().getDisplayMetrics();
        float width = Math.max(0, metrics.widthPixels - 1);
        float height = Math.max(0, metrics.heightPixels - 1);
        float diagonal = (float) Math.hypot(metrics.widthPixels, metrics.heightPixels);
        int source = InputDevice.SOURCE_TOUCHSCREEN;
        return new InputDevice.Builder()
                .setId(TOUCHSCREEN_ID)
                .setName("Touchscreen")
                .setDescriptor("minato-touchscreen")
                .setSources(source)
                .addMotionRange(MotionEvent.AXIS_X, source, 0, width, 0, 0, 0)
                .addMotionRange(MotionEvent.AXIS_Y, source, 0, height, 0, 0, 0)
                .addMotionRange(MotionEvent.AXIS_PRESSURE, source, 0, 1, 0, 0, 0)
                .addMotionRange(MotionEvent.AXIS_TOUCH_MAJOR, source, 0, diagonal, 0, 0, 0)
                .addMotionRange(MotionEvent.AXIS_TOUCH_MINOR, source, 0, diagonal, 0, 0, 0)
                .addMotionRange(MotionEvent.AXIS_TOOL_MAJOR, source, 0, diagonal, 0, 0, 0)
                .addMotionRange(MotionEvent.AXIS_TOOL_MINOR, source, 0, diagonal, 0, 0, 0)
                .setKeyCharacterMap(KeyCharacterMap.obtainEmptyMap(TOUCHSCREEN_ID))
                .build();
    }

    /** The virtual keyboard's key character map, Virtual.kcm's, which libandroid_runtime holds (ADR-0045 step 7c1). */
    private static native KeyCharacterMap nativeVirtualKeyCharacterMap();

    /** A hardware keyboard's key character map, Generic.kcm's, for the device `deviceId` (ADR-0068 step 7c2). */
    private static native KeyCharacterMap nativeKeyboardKeyCharacterMap(int deviceId);
}
