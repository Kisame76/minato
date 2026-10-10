package io.github.kisame76.minato.test.hardwarebuffer;

import android.app.Application;
import android.content.Context;
import android.hardware.HardwareBuffer;
import android.os.Parcel;
import android.util.Log;

/**
 * ADR-0058 a)'s test of android.hardware.HardwareBuffer, AOSP's class over Minato's JNI on libnativewindow's
 * AHardwareBuffer: from Application.attachBaseContext it asks isSupported, creates buffers and logs their getters,
 * the descriptions Android refuses with their exceptions, a Parcel round trip, close and what a closed buffer
 * answers, and libandroid's AHardwareBuffer_fromHardwareBuffer and toHardwareBuffer through its JNI library. No value
 * of a device's gralloc alone is logged: an ID as "not 0" and "the same"; create with protected content and the CPU
 * is left out, which create hands to gralloc without AHardwareBuffer's rules and the emulator's gralloc makes. Only
 * public API, as an app has it on Android. It logs with the tag hardware-buffer-test and ends with "done"; tools/android-reference.sh compares it
 * with fixtures/expected/hardware-buffer-test.logcat.
 */
public class HardwareBufferApplication extends Application {
    private static final String TAG = "hardware-buffer-test";
    private static final long CPU = HardwareBuffer.USAGE_CPU_READ_OFTEN | HardwareBuffer.USAGE_CPU_WRITE_OFTEN;

    static {
        System.loadLibrary("hardware-buffer-test");
    }

    private static native String fromJava(HardwareBuffer buffer, long id);

    private static native HardwareBuffer toJava();

    private static native int firstWord(HardwareBuffer buffer);

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        supported();
        oneBuffer();
        refused();
        parcel();
        closed();
        nativeSides();
        Log.i(TAG, "done");
    }

    private static String describe(HardwareBuffer buffer) {
        return buffer.getWidth() + " x " + buffer.getHeight() + " x " + buffer.getLayers() + ", format "
                + buffer.getFormat() + ", usage 0x" + Long.toHexString(buffer.getUsage());
    }

    private static void supported() {
        Log.i(TAG, "isSupported RGBA_8888: "
                + HardwareBuffer.isSupported(64, 48, HardwareBuffer.RGBA_8888, 1,
                        CPU | HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE));
        Log.i(TAG, "isSupported RGB_565: " + HardwareBuffer.isSupported(64, 48, HardwareBuffer.RGB_565, 1, CPU));
        Log.i(TAG, "isSupported BLOB: " + HardwareBuffer.isSupported(4096, 1, HardwareBuffer.BLOB, 1, CPU));
        Log.i(TAG, "isSupported RGBA_8888 protected with the CPU: " + HardwareBuffer.isSupported(64, 48,
                HardwareBuffer.RGBA_8888, 1, CPU | HardwareBuffer.USAGE_PROTECTED_CONTENT));
    }

    private static void oneBuffer() {
        HardwareBuffer buffer = HardwareBuffer.create(64, 48, HardwareBuffer.RGBA_8888, 1,
                CPU | HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE);
        HardwareBuffer other = HardwareBuffer.create(64, 48, HardwareBuffer.RGBA_8888, 1, CPU);
        Log.i(TAG, "create: " + describe(buffer) + ", closed " + buffer.isClosed());
        Log.i(TAG, "getId: not 0 " + (buffer.getId() != 0) + ", another buffer's differs "
                + (other.getId() != buffer.getId()));
        HardwareBuffer blob = HardwareBuffer.create(100000, 1, HardwareBuffer.BLOB, 1, CPU);
        Log.i(TAG, "a BLOB: " + describe(blob));
        other.close();
        blob.close();
        buffer.close();
    }

    private static void refused() {
        int[][] sizes = {{0, 48, HardwareBuffer.RGBA_8888, 1}, {64, 0, HardwareBuffer.RGBA_8888, 1},
                {64, 48, HardwareBuffer.RGBA_8888, 0}, {64, 48, 0x99, 1}, {4096, 2, HardwareBuffer.BLOB, 1}};
        for (int[] size : sizes) {
            try {
                HardwareBuffer.create(size[0], size[1], size[2], size[3], CPU).close();
                Log.i(TAG, "create " + size[0] + " x " + size[1] + ", format " + size[2] + ", layers " + size[3]
                        + ": made");
            } catch (RuntimeException e) {
                Log.i(TAG, "create " + size[0] + " x " + size[1] + ", format " + size[2] + ", layers " + size[3]
                        + ": " + e.getClass().getName() + ": " + e.getMessage());
            }
        }
    }

    private static void parcel() {
        HardwareBuffer buffer = HardwareBuffer.create(16, 8, HardwareBuffer.RGBA_8888, 1, CPU);
        Parcel parcel = Parcel.obtain();
        buffer.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        HardwareBuffer read = HardwareBuffer.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        Log.i(TAG, "from a parcel: " + describe(read) + ", the same ID " + (read.getId() == buffer.getId())
                + ", describeContents " + buffer.describeContents());
        buffer.close();
        Log.i(TAG, "the read one after the written one's close: " + describe(read));
        read.close();
    }

    private static void closed() {
        HardwareBuffer buffer = HardwareBuffer.create(16, 8, HardwareBuffer.RGBA_8888, 1, CPU);
        buffer.close();
        buffer.close();
        Log.i(TAG, "closed twice: closed " + buffer.isClosed());
        try {
            Log.i(TAG, "getWidth after close: " + buffer.getWidth());
        } catch (RuntimeException e) {
            Log.i(TAG, "getWidth after close: " + e.getClass().getName() + ": " + e.getMessage());
        }
        try {
            buffer.writeToParcel(Parcel.obtain(), 0);
            Log.i(TAG, "writeToParcel after close: written");
        } catch (RuntimeException e) {
            Log.i(TAG, "writeToParcel after close: " + e.getClass().getName() + ": " + e.getMessage());
        }
    }

    private static void nativeSides() {
        HardwareBuffer buffer = HardwareBuffer.create(64, 48, HardwareBuffer.RGBA_8888, 1, CPU);
        Log.i(TAG, "fromHardwareBuffer: " + fromJava(buffer, buffer.getId()));
        Log.i(TAG, "the pattern read back natively: 0x" + Integer.toHexString(firstWord(buffer)));
        buffer.close();
        HardwareBuffer made = toJava();
        Log.i(TAG, "toHardwareBuffer: " + (made == null ? "null" : describe(made)));
        if (made != null) made.close();
    }
}
