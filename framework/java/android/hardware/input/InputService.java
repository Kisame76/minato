/*
 * The input service (ADR-0045 decisions 2, 3 and 6; gap 7a of the Godot survey): the IInputManager of
 * InputManagerService, local in the app's process, which ServiceManager's table gives as "input". It is AIDL's Default
 * class, as the display service is. Its answers:
 *   - getInputDeviceIds and getInputDevice: Minato's devices, built with InputDevice.Builder.
 *     - The virtual keyboard, id -1, as Android's EventHub makes it (createVirtualKeyboardLocked): "Virtual", a
 *       keyboard with letters and a d-pad. Its key character map is step 7c's; until then it has none.
 *     - The touchscreen, id 1, M3's input device (runtime/android/input.cpp:17): a finger's coordinates on the
 *       session's screen, its pressure and its touch and tool sizes, the axes M3's events carry.
 *   - registerInputDevicesChangedListener keeps the listener: no device comes or goes yet.
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
import android.view.KeyCharacterMap;
import android.view.MotionEvent;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;

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

    private final Binder mBinder = new Binder();
    private final List<IInputDevicesChangedListener> mListeners = new ArrayList<>();
    private InputDevice[] mDevices;

    private InputService() {
        mBinder.attachInterface(this, IInputManager.Stub.DESCRIPTOR);
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
        return mDevices;
    }

    /** EventHub's virtual keyboard: KEYBOARD, ALPHAKEY, DPAD and VIRTUAL, which InputReader reports as below. */
    private static InputDevice virtualKeyboard() {
        return new InputDevice.Builder()
                .setId(KeyCharacterMap.VIRTUAL_KEYBOARD)
                .setName("Virtual")
                .setDescriptor(VIRTUAL_KEYBOARD_DESCRIPTOR)
                .setSources(InputDevice.SOURCE_KEYBOARD | InputDevice.SOURCE_DPAD)
                .setKeyboardType(InputDevice.KEYBOARD_TYPE_ALPHABETIC)
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
                .build();
    }
}
