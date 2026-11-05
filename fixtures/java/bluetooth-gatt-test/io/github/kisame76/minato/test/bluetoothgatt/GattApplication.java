package io.github.kisame76.minato.test.bluetoothgatt;

import android.app.Application;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattServerCallback;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.UUID;
import java.util.concurrent.Callable;

/**
 * Bluetooth's GATT classes and the adapter's answers for an app that holds no Bluetooth permission: the services,
 * characteristics and descriptors an app builds, the callback it subclasses, the remote device it asks for by address,
 * and the connection it tries on an adapter that is off. Everything is reached by name, so that the same sources build
 * against Android's SDK and against Minato's framework before it has the classes. Each line is the result or the class
 * of the exception it throws.
 */
public class GattApplication extends Application {
    private static final String TAG = "bluetooth-gatt-test";
    private static final String BT = "android.bluetooth.";
    private static final UUID SERVICE = UUID.fromString("0000180d-0000-1000-8000-00805f9b34fb");
    private static final UUID CHARACTERISTIC = UUID.fromString("00002a37-0000-1000-8000-00805f9b34fb");
    private static final UUID DESCRIPTOR = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    /** A client callback of an app, which overrides what it hears and leaves the rest to the base class. */
    private static final class Callback extends BluetoothGattCallback {
        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicRead(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic, int status) {
            Log.i(TAG, "callback onCharacteristicRead (deprecated) status " + status);
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
            Log.i(TAG, "callback onCharacteristicChanged (deprecated)");
        }

        @Override
        @SuppressWarnings("deprecation")
        public void onDescriptorRead(BluetoothGatt gatt, BluetoothGattDescriptor descriptor, int status) {
            Log.i(TAG, "callback onDescriptorRead (deprecated) status " + status);
        }
    }

    private static void log(String name, Callable<Object> call) {
        log(TAG, name, call);
    }

    private static void log(String tag, String name, Callable<Object> call) {
        String result;
        try {
            final Object value = call.call();
            result = value instanceof byte[] ? Arrays.toString((byte[]) value) : String.valueOf(value);
        } catch (InvocationTargetException e) {
            result = "throws " + e.getCause().getClass().getName();
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(tag, name + ": " + result);
    }

    private static Class<?> boxed(Class<?> type) {
        if (type == int.class) return Integer.class;
        if (type == boolean.class) return Boolean.class;
        if (type == long.class) return Long.class;
        if (type == float.class) return Float.class;
        if (type == byte.class) return Byte.class;
        return type;
    }

    private static boolean accepts(Class<?>[] types, Object[] args) {
        if (types.length != args.length) return false;
        for (int i = 0; i < types.length; i++) {
            if (args[i] == null ? types[i].isPrimitive() : !boxed(types[i]).isInstance(args[i])) return false;
        }
        return true;
    }

    private static Object call(Object target, String method, Object... args) throws Exception {
        for (Method candidate : target.getClass().getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate.getParameterTypes(), args)) {
                return candidate.invoke(target, args);
            }
        }
        throw new NoSuchMethodException(target.getClass().getName() + "." + method);
    }

    private static Object callStatic(String type, String method, Object... args) throws Exception {
        for (Method candidate : Class.forName(type).getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate.getParameterTypes(), args)) {
                return candidate.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(type + "." + method);
    }

    private static Object create(String type, Object... args) throws Exception {
        for (Constructor<?> candidate : Class.forName(type).getConstructors()) {
            if (accepts(candidate.getParameterTypes(), args)) return candidate.newInstance(args);
        }
        throw new NoSuchMethodException(type + " constructor");
    }

    private static Object field(String type, String name) throws Exception {
        return Class.forName(type).getField(name).get(null);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        try {
            runAll();
        } catch (Throwable e) {
            Log.i(TAG, "failed: " + e);
        }
    }

    private void runAll() throws Exception {
        final Context context = this;
        for (String name : new String[] {"BluetoothGatt", "BluetoothGattCallback", "BluetoothGattService",
                "BluetoothGattCharacteristic", "BluetoothGattDescriptor", "BluetoothGattServer",
                "BluetoothGattServerCallback", "BluetoothSocket", "BluetoothServerSocket", "BluetoothClass",
                "le.BluetoothLeScanner", "le.BluetoothLeAdvertiser", "le.ScanCallback", "le.ScanSettings",
                "le.ScanResult", "le.AdvertiseCallback"}) {
            log("class " + name, () -> Class.forName(BT + name).getSimpleName());
        }

        final Object manager = getSystemService(Context.BLUETOOTH_SERVICE);
        final Object adapter = manager == null ? null : call(manager, "getAdapter");
        log(TAG + "-device", "adapter state", () -> call(adapter, "getState"));
        log(TAG + "-device", "adapter isEnabled", () -> call(adapter, "isEnabled"));
        log("adapter getName", () -> call(adapter, "getName"));
        log("adapter getBondedDevices", () -> call(adapter, "getBondedDevices"));
        log(TAG + "-device", "adapter getBluetoothLeScanner", () -> call(adapter, "getBluetoothLeScanner"));
        log(TAG + "-device", "adapter getBluetoothLeAdvertiser", () -> call(adapter, "getBluetoothLeAdvertiser"));
        log(TAG + "-device", "adapter isMultipleAdvertisementSupported",
                () -> call(adapter, "isMultipleAdvertisementSupported"));
        log(TAG + "-device", "adapter isLe2MPhySupported", () -> call(adapter, "isLe2MPhySupported"));
        log("adapter getAddress", () -> call(adapter, "getAddress"));
        log("adapter getScanMode", () -> call(adapter, "getScanMode"));
        log(TAG + "-device", "adapter enable", () -> call(adapter, "enable"));
        log("adapter disable", () -> call(adapter, "disable"));
        log("adapter getProfileConnectionState", () -> call(adapter, "getProfileConnectionState", 1));
        log("adapter startDiscovery", () -> call(adapter, "startDiscovery"));
        log("adapter isDiscovering", () -> call(adapter, "isDiscovering"));
        log("adapter listenUsingRfcommWithServiceRecord", () -> call(adapter, "listenUsingRfcommWithServiceRecord",
                "x", SERVICE));
        log("checkBluetoothAddress good", () -> callStatic(BT + "BluetoothAdapter", "checkBluetoothAddress",
                "00:11:22:33:44:55"));
        log("checkBluetoothAddress lower case", () -> callStatic(BT + "BluetoothAdapter", "checkBluetoothAddress",
                "00:11:22:33:44:ab"));
        log("checkBluetoothAddress bad", () -> callStatic(BT + "BluetoothAdapter", "checkBluetoothAddress", "bad"));
        log("manager getConnectionState", () -> {
            final Object device = call(adapter, "getRemoteDevice", "00:11:22:33:44:55");
            return call(manager, "getConnectionState", device, 7);
        });
        log("manager getConnectedDevices GATT_SERVER", () -> call(manager, "getConnectedDevices", 8));
        log("manager openGattServer null callback", () -> call(manager, "openGattServer", context, null));

        final Object device = call(adapter, "getRemoteDevice", "00:11:22:33:44:55");
        log("getRemoteDevice address", () -> call(device, "getAddress"));
        log("getRemoteDevice toString", () -> device.toString());
        log("getRemoteDevice equals", () -> device.equals(call(adapter, "getRemoteDevice", "00:11:22:33:44:55")));
        log("getRemoteDevice lower case", () -> call(adapter, "getRemoteDevice", "00:11:22:33:44:ab"));
        log("getRemoteDevice bad", () -> call(adapter, "getRemoteDevice", "bad"));
        log("getRemoteDevice null", () -> call(adapter, "getRemoteDevice", (Object) null));
        log("device getName", () -> call(device, "getName"));
        log("device getType", () -> call(device, "getType"));
        log("device getBondState", () -> call(device, "getBondState"));
        log("device getBluetoothClass", () -> call(device, "getBluetoothClass"));
        log("device getUuids", () -> call(device, "getUuids"));
        log("device connectGatt null callback", () -> call(device, "connectGatt", context, false, null));
        log("device createRfcommSocketToServiceRecord is not null", () -> call(device,
                "createRfcommSocketToServiceRecord", SERVICE) != null);
        log("device DEVICE_TYPE_LE", () -> field(BT + "BluetoothDevice", "DEVICE_TYPE_LE"));
        log("device BOND_NONE", () -> field(BT + "BluetoothDevice", "BOND_NONE"));

        log("gatt constants", () -> field(BT + "BluetoothGatt", "GATT_SUCCESS") + " "
                + field(BT + "BluetoothGatt", "GATT_FAILURE") + " "
                + field(BT + "BluetoothGatt", "GATT_WRITE_NOT_PERMITTED") + " "
                + field(BT + "BluetoothGatt", "CONNECTION_PRIORITY_HIGH") + " "
                + field(BT + "BluetoothProfile", "STATE_CONNECTED") + " " + field(BT + "BluetoothProfile", "GATT"));
        log("callback is abstract", () -> Modifier.isAbstract(Class.forName(BT + "BluetoothGattCallback")
                .getModifiers()));
        log("callback methods", () -> {
            final Method[] methods = Class.forName(BT + "BluetoothGattCallback").getDeclaredMethods();
            final String[] names = new String[methods.length];
            for (int i = 0; i < names.length; i++) names[i] = methods[i].getName();
            Arrays.sort(names);
            return Arrays.toString(names);
        });
        log("server callback methods", () -> Class.forName(BT + "BluetoothGattServerCallback")
                .getDeclaredMethods().length);

        log("service type", () -> call(create(BT + "BluetoothGattService", SERVICE, 0), "getType"));
        log("service uuid", () -> call(create(BT + "BluetoothGattService", SERVICE, 0), "getUuid"));
        log("service instanceId", () -> call(create(BT + "BluetoothGattService", SERVICE, 0), "getInstanceId"));
        log("service characteristics empty", () -> call(call(create(BT + "BluetoothGattService", SERVICE, 1),
                "getCharacteristics"), "size"));
        log("service addCharacteristic", () -> {
            final Object service = create(BT + "BluetoothGattService", SERVICE, 0);
            final Object characteristic = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            final Object added = call(service, "addCharacteristic", characteristic);
            return added + " " + call(call(service, "getCharacteristics"), "size") + " "
                    + (call(service, "getCharacteristic", CHARACTERISTIC) == characteristic) + " "
                    + (call(characteristic, "getService") == service);
        });
        log("service addService", () -> {
            final Object service = create(BT + "BluetoothGattService", SERVICE, 0);
            final Object included = create(BT + "BluetoothGattService", SERVICE, 1);
            return call(service, "addService", included) + " " + call(call(service, "getIncludedServices"), "size");
        });
        log("characteristic fields", () -> {
            final Object c = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            return call(c, "getUuid") + " " + call(c, "getProperties") + " " + call(c, "getPermissions") + " "
                    + call(c, "getWriteType") + " " + call(c, "getInstanceId") + " " + call(c, "getValue") + " "
                    + call(c, "getService");
        });
        log("characteristic setValue bytes", () -> {
            final Object c = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            final Object ok = call(c, "setValue", new byte[] {1, 2, 3});
            return ok + " " + Arrays.toString((byte[]) call(c, "getValue"));
        });
        log("characteristic setValue int", () -> {
            final Object c = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            final Object ok = call(c, "setValue", 0x1234, 18, 0);
            return ok + " " + Arrays.toString((byte[]) call(c, "getValue")) + " " + call(c, "getIntValue", 18, 0)
                    + " " + call(c, "getIntValue", 17, 1);
        });
        log("characteristic setValue string", () -> {
            final Object c = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            call(c, "setValue", "hi");
            return call(c, "getStringValue", 0) + " " + call(c, "getStringValue", 1);
        });
        log("characteristic setValue float", () -> {
            final Object c = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            call(c, "setValue", 7, 0, 50, 0);
            return Arrays.toString((byte[]) call(c, "getValue")) + " " + call(c, "getFloatValue", 50, 0);
        });
        log("characteristic setWriteType", () -> {
            final Object c = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            call(c, "setWriteType", 1);
            return call(c, "getWriteType");
        });
        log("characteristic descriptor", () -> {
            final Object c = create(BT + "BluetoothGattCharacteristic", CHARACTERISTIC, 0x12, 0x01);
            final Object d = create(BT + "BluetoothGattDescriptor", DESCRIPTOR, 0x11);
            final Object added = call(c, "addDescriptor", d);
            return added + " " + call(call(c, "getDescriptors"), "size") + " "
                    + (call(c, "getDescriptor", DESCRIPTOR) == d) + " " + (call(d, "getCharacteristic") == c) + " "
                    + call(d, "getPermissions") + " " + call(d, "getUuid") + " " + call(d, "getValue");
        });
        log("descriptor setValue", () -> {
            final Object d = create(BT + "BluetoothGattDescriptor", DESCRIPTOR, 0x11);
            final Object ok = call(d, "setValue", field(BT + "BluetoothGattDescriptor", "ENABLE_NOTIFICATION_VALUE"));
            return ok + " " + Arrays.toString((byte[]) call(d, "getValue"));
        });
        log("descriptor value constants", () -> Arrays.toString((byte[]) field(BT + "BluetoothGattDescriptor",
                "ENABLE_INDICATION_VALUE")) + " " + Arrays.toString((byte[]) field(BT + "BluetoothGattDescriptor",
                "DISABLE_NOTIFICATION_VALUE")));
        log("characteristic constants", () -> field(BT + "BluetoothGattCharacteristic", "PROPERTY_NOTIFY") + " "
                + field(BT + "BluetoothGattCharacteristic", "FORMAT_UINT16") + " "
                + field(BT + "BluetoothGattCharacteristic", "WRITE_TYPE_NO_RESPONSE"));
        log("service constants", () -> field(BT + "BluetoothGattService", "SERVICE_TYPE_PRIMARY") + " "
                + field(BT + "BluetoothGattService", "SERVICE_TYPE_SECONDARY"));
        final Callback callback = new Callback();
        callback.onCharacteristicRead(null, null, new byte[] {1}, 5);
        callback.onCharacteristicChanged(null, null, new byte[] {1});
        callback.onDescriptorRead(null, null, 7, new byte[] {1});
        callback.onConnectionStateChange(null, 0, 2);
        callback.onServicesDiscovered(null, 0);
        log("device connectGatt", () -> call(device, "connectGatt", context, false, callback));
        log("device connectGatt transport", () -> call(device, "connectGatt", context, false, callback, 2));
        final BluetoothGattServerCallback serverCallback = new BluetoothGattServerCallback() { };
        serverCallback.onServiceAdded(0, null);
        log("manager openGattServer", () -> call(manager, "openGattServer", context, serverCallback));
        Log.i(TAG, "done");
    }
}
