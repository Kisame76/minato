package io.github.kisame76.minato.test.breadth;

import android.app.Application;
import android.os.Debug;
import android.util.Base64;
import android.util.Base64DataException;
import android.util.Base64InputStream;
import android.util.Base64OutputStream;
import android.util.Log;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.GZIPInputStream;
import java.util.zip.GZIPOutputStream;

/**
 * The framework breadth's behavior test, batch by batch, without a window. It logs under the tag breadth-test what the
 * batches' types answer. Batch 3a, the Unity test APK probe's types:
 * <ul>
 * <li>debug: what Crashlytics asks before it reports a crash (isDebuggerConnected and waitingForDebugger), with no
 * debugger attached, and an empty MemoryInfo;
 * <li>base64: Base64OutputStream and Base64InputStream with Base64's flags, written in pieces, a long input's lines,
 * the URL-safe, unwrapped and unpadded chain over GZIP that Firebase's heartbeat header writes, and a bad input's
 * exception, after a character outside the alphabet that it skips.
 * <li>debug natives (batch 3b): Debug's public memory numbers as relations that hold on any device (an app uses some
 * heap, the heap sizes add up, a MemoryInfo has its parts and Pss), and that none of its hidden natives lacks its
 * implementation, with their values only shown.
 * <li>network types: what an app gets from IpConfiguration, StaticIpConfiguration and CaptivePortal
 * (system API: the answers an app gets), the hidden constructor of CaptivePortal (only shown), the answers of
 * SSLCertificateSocketFactory, and SensorPrivacyManager's constants read at run time.
 * </ul>
 * A failing part logs "failed" and its stack under breadth-test-device, which differs by device and is only shown.
 * tools/android-reference.sh takes the log from Android 15, and tools/breadth-check.sh compares Minato's with it.
 */
public class BreadthApplication extends Application {
    private static final String TAG = "breadth-test";

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("debug", BreadthApplication::debug);
        part("base64", BreadthApplication::base64);
        part("debug natives", BreadthApplication::debugNatives);
        part("hidden natives", BreadthApplication::hiddenNatives);
        part("network types", BreadthApplication::networkTypes);
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    /** What Crashlytics asks before it reports a crash, and an empty MemoryInfo. */
    private static void debug() {
        Log.i(TAG, "debug: debugger connected " + Debug.isDebuggerConnected() + ", waiting for one "
                + Debug.waitingForDebugger());
        Debug.MemoryInfo info = new Debug.MemoryInfo();
        Log.i(TAG, "debug: an empty MemoryInfo's total PSS " + info.getTotalPss() + ", its summary's "
                + info.getMemoryStat("summary.total-pss") + ", its Java heap's "
                + info.getMemoryStat("summary.java-heap"));
        // The thread's CPU time differs by device and run, so it is only shown.
        Log.i(TAG + "-device", "debug: thread CPU time " + Debug.threadCpuTimeNanos() + " ns");
    }

    /** The Base64 streams with each of Base64's flags, a long input and Firebase's chain over GZIP. */
    private static void base64() throws IOException {
        byte[] data = "Minato, the harbor: \u6e2f".getBytes(StandardCharsets.UTF_8);
        int[] flags = {Base64.DEFAULT, Base64.NO_WRAP, Base64.NO_PADDING | Base64.NO_WRAP,
                Base64.URL_SAFE | Base64.NO_WRAP, Base64.CRLF};
        for (int flag : flags) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (OutputStream out = new Base64OutputStream(bytes, flag)) {
                // In pieces, so the stream carries the bytes that make no whole group.
                out.write(data, 0, 5);
                out.write(data[5]);
                out.write(data, 6, data.length - 6);
            }
            byte[] encoded = bytes.toByteArray();
            byte[] back = readAll(new Base64InputStream(new ByteArrayInputStream(encoded), flag));
            Log.i(TAG, "base64: flags 0x" + Integer.toHexString(flag) + ": " + shown(encoded) + ", back equal "
                    + Arrays.equals(data, back) + ", same as Base64.encode " + Arrays.equals(encoded,
                    Base64.encode(data, flag)));
        }
        byte[] counting = new byte[100];
        for (int i = 0; i < counting.length; i++) {
            counting[i] = (byte) i;
        }
        ByteArrayOutputStream lines = new ByteArrayOutputStream();
        try (OutputStream out = new Base64OutputStream(lines, Base64.DEFAULT)) {
            out.write(counting);
        }
        Log.i(TAG, "base64: 100 bytes: " + shown(lines.toByteArray()));
        int firebase = Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING;
        ByteArrayOutputStream header = new ByteArrayOutputStream();
        try (OutputStream out = new GZIPOutputStream(new Base64OutputStream(header, firebase))) {
            out.write("{\"heartbeats\":[{\"agent\":\"fire-core/20.3.0\",\"dates\":[\"2026-10-08\"]}]}"
                    .getBytes(StandardCharsets.UTF_8));
        }
        String text = header.toString("US-ASCII");
        // GZIP's deflated bytes may differ by zlib, so only the alphabet and the way back are compared.
        byte[] json = readAll(new GZIPInputStream(new Base64InputStream(new ByteArrayInputStream(header.toByteArray()),
                firebase)));
        Log.i(TAG, "base64: Firebase's header URL-safe " + text.matches("[A-Za-z0-9_-]+") + ", back "
                + new String(json, StandardCharsets.UTF_8));
        byte[] skipped = readAll(new Base64InputStream(
                new ByteArrayInputStream("TW!lu".getBytes(StandardCharsets.US_ASCII)), Base64.DEFAULT));
        Log.i(TAG, "base64: a character outside the alphabet skipped: " + new String(skipped, StandardCharsets.UTF_8));
        try {
            // A character outside the alphabet is skipped, as Base64 does; padding first is not.
            readAll(new Base64InputStream(new ByteArrayInputStream("=abc".getBytes(StandardCharsets.US_ASCII)),
                    Base64.DEFAULT));
            Log.i(TAG, "base64: a bad input read");
        } catch (Base64DataException e) {
            Log.i(TAG, "base64: a bad input: " + e.getClass().getName() + ": " + e.getMessage());
        }
    }

    /** The natives of Debug's public API: relations between the numbers that hold on any device. */
    private static void debugNatives() throws Exception {
        long size = Debug.getNativeHeapSize();
        long allocated = Debug.getNativeHeapAllocatedSize();
        long free = Debug.getNativeHeapFreeSize();
        Log.i(TAG, "debug natives: native heap in use " + (allocated > 0) + ", in use within size "
                + (allocated <= size) + ", free within size " + (free >= 0 && free <= size));
        Debug.MemoryInfo info = new Debug.MemoryInfo();
        Debug.getMemoryInfo(info);
        Log.i(TAG, "debug natives: MemoryInfo native " + (info.nativePss > 0) + ", Java heap " + (info.dalvikPss > 0)
                + ", total " + (info.getTotalPss() > 0) + ", total holds the parts "
                + (info.getTotalPss() >= info.dalvikPss + info.nativePss + info.otherPss) + ", private dirty "
                + (info.getTotalPrivateDirty() > 0));
        Log.i(TAG, "debug natives: own Pss " + (Debug.getPss() > 0));
        // The binder statistics differ by device and are only shown, but each native must run.
        Log.i(TAG + "-device", "debug natives: binder " + Debug.getBinderSentTransactions() + " "
                + Debug.getBinderReceivedTransactions() + " " + Debug.getBinderLocalObjectCount() + " "
                + Debug.getBinderProxyObjectCount() + " " + Debug.getBinderDeathObjectCount());
    }

    /**
     * The natives that only the hidden API has. Android 15 refuses an app a hidden method (NoSuchMethodError), so the
     * reference only shows that none of them lacks its native; Minato's answers are shown.
     */
    private static void hiddenNatives() {
        int linked = 0;
        int unlinked = 0;
        String[] names = {"getRss", "getZramFreeKb", "getIonHeapsSizeKb", "getIonPoolsSizeKb",
                "getDmabufTotalExportedKb",
                "getDmabufHeapTotalExportedKb", "getDmabufHeapPoolsSizeKb", "getDmabufMappedSizeKb",
                "getGpuPrivateMemoryKb", "getGpuTotalUsageKb"};
        StringBuilder shown = new StringBuilder();
        for (String name : names) {
            try {
                shown.append(' ').append(name).append(' ').append(Debug.class.getMethod(name).invoke(null));
                linked++;
            } catch (java.lang.reflect.InvocationTargetException e) {
                if (e.getCause() instanceof UnsatisfiedLinkError) {
                    unlinked++;
                } else {
                    linked++;
                }
            } catch (ReflectiveOperationException e) {
                // Android 15's hidden API refusal.
                linked++;
            }
        }
        try {
            long[] system = new long[26];
            Object[] arguments = {system};
            Debug.class.getMethod("getMemInfo", long[].class).invoke(null, arguments);
            shown.append(" total ").append(system[0]).append(" free ").append(system[1]);
            Debug.MemoryInfo other = new Debug.MemoryInfo();
            shown.append(" other process ").append(Debug.class.getMethod("getMemoryInfo", int.class,
                    Debug.MemoryInfo.class).invoke(null, 1, other));
            shown.append(" pss ").append(Debug.class.getMethod("getPss", int.class, long[].class, long[].class)
                    .invoke(null, android.os.Process.myPid(), new long[3], new long[4]));
            shown.append(" vmap ").append(Debug.class.getMethod("isVmapStack").invoke(null));
            shown.append(" unreachable ").append(Debug.class.getMethod("getUnreachableMemory", int.class,
                    boolean.class).invoke(null, 1, false));
            linked++;
        } catch (java.lang.reflect.InvocationTargetException e) {
            if (e.getCause() instanceof UnsatisfiedLinkError) {
                unlinked++;
            }
        } catch (ReflectiveOperationException e) {
            linked++;
        }
        Log.i(TAG, "debug natives: every hidden native is linked " + (unlinked == 0));
        Log.i(TAG + "-device", "debug natives: hidden" + shown);
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try (InputStream stream = in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[64];
            for (int count; (count = stream.read(buffer)) > 0; ) {
                out.write(buffer, 0, count);
            }
            return out.toByteArray();
        }
    }

    /** Encoded bytes as text, with line ends shown. */
    /** The result of `call`, or the class of what it threw: a hidden API's refusal is part of the answer. */
    private interface Call {
        Object run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (java.lang.reflect.InvocationTargetException e) {
            return "threw " + e.getCause().getClass().getName();
        } catch (Throwable t) {
            return "threw " + t.getClass().getName();
        }
    }

    /** IpConfiguration, StaticIpConfiguration, CaptivePortal, SSLCertificateSocketFactory, SensorPrivacyManager. */
    private static void networkTypes() throws Exception {
        for (String name : new String[] {"android.net.IpConfiguration", "android.net.StaticIpConfiguration"}) {
            Class<?> type = Class.forName(name);
            Log.i(TAG, "network types: " + name + " new " + outcome(() -> type.getConstructor().newInstance() != null));
            Log.i(TAG, "network types: " + name + " builder or setters "
                    + outcome(() -> type.getMethod("toString") != null));
        }
        // CaptivePortal: public methods on a portal made over a Binder that logs what it is asked.
        final StringBuilder asked = new StringBuilder();
        android.os.Binder binder = new android.os.Binder() {
            @Override
            protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags) {
                data.enforceInterface("android.net.ICaptivePortal");
                asked.append(" code ").append(code).append(" arg ").append(data.readInt());
                return true;
            }
        };
        // The refusal of its hidden constructor is Android 15's (NoSuchMethodException); Minato's answer is only shown.
        Log.i(TAG + "-device", "network types: CaptivePortal new " + outcome(() -> {
            Object portal = Class.forName("android.net.CaptivePortal").getConstructor(android.os.IBinder.class)
                    .newInstance(binder);
            android.net.CaptivePortal.class.getMethod("ignoreNetwork").invoke(portal);
            android.net.CaptivePortal.class.getMethod("reportCaptivePortalDismissed").invoke(portal);
            return "asked" + asked;
        }));
        // SSLCertificateSocketFactory: what is the same on every device, as relations.
        android.net.SSLCertificateSocketFactory factory = (android.net.SSLCertificateSocketFactory)
                android.net.SSLCertificateSocketFactory.getDefault(1000, null);
        Log.i(TAG, "network types: SSLCertificateSocketFactory default suites "
                + (factory.getDefaultCipherSuites().length > 0) + ", supported "
                + (factory.getSupportedCipherSuites().length > 0));
        Log.i(TAG, "network types: SSLCertificateSocketFactory insecure is one of its own class "
                + (android.net.SSLCertificateSocketFactory.getInsecure(1000, null) instanceof
                        android.net.SSLCertificateSocketFactory));
        // SensorPrivacyManager's constants, read at run time from the class (the app's own compile inlines its stubs).
        Class<?> sensors = Class.forName("android.hardware.SensorPrivacyManager$Sensors");
        Class<?> manager = Class.forName("android.hardware.SensorPrivacyManager");
        Log.i(TAG, "network types: SensorPrivacyManager sensors microphone "
                + outcome(() -> sensors.getField("MICROPHONE").getInt(null)) + " camera "
                + outcome(() -> sensors.getField("CAMERA").getInt(null)) + ", toggle types software "
                + outcome(() -> manager.getField("TOGGLE_TYPE_SOFTWARE").getInt(null)) + " hardware "
                + outcome(() -> manager.getField("TOGGLE_TYPE_HARDWARE").getInt(null)));
    }

    private static String shown(byte[] encoded) {
        return new String(encoded, StandardCharsets.US_ASCII).replace("\r", "\\r").replace("\n", "\\n");
    }
}
