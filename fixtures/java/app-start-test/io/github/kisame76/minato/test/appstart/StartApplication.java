package io.github.kisame76.minato.test.appstart;

import android.app.Application;
import android.os.Process;
import android.system.Os;
import android.util.Log;
import java.io.FileInputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.util.Map;
import java.util.TreeMap;

/**
 * ADR-0039's test 1: what an app process looks like from the inside, printed from the Application class's static
 * initializer, which runs when the process loads it, before any Context exists. System.out reaches logcat as
 * "System.out" on Android (RuntimeInit.redirectLogStreams) and the host's log in Minato.
 */
public class StartApplication extends Application {
    static {
        report();
    }

    static void report() {
        for (Map.Entry<String, String> entry : new TreeMap<>(System.getenv()).entrySet()) {
            System.out.println("app-start-test: env " + entry.getKey() + "=" + entry.getValue());
        }
        System.out.println("app-start-test: cmdline " + cmdline());
        System.out.println("app-start-test: getuid " + Os.getuid());
        System.out.println("app-start-test: Process.myUid " + Process.myUid());
        System.out.println("app-start-test: currentProcessName " + currentProcessName());
        System.out.println("app-start-test: MessageDigest " + sha256("minato"));
        // Log with a Throwable looks for a DeadSystemException among its causes (Log.printlns), so that class must be
        // in framework.jar. Its own tag keeps the lines out of those compared with Android's.
        Log.e("app-start-test-log", "a Throwable", new Exception("logged"));
    }

    /** /proc/self/cmdline, its NUL-separated arguments joined with spaces. */
    private static String cmdline() {
        try (FileInputStream in = new FileInputStream("/proc/self/cmdline")) {
            byte[] buffer = new byte[4096];
            int length = 0;
            for (int read; (read = in.read(buffer, length, buffer.length - length)) > 0; ) {
                length += read;
            }
            while (length > 0 && buffer[length - 1] == 0) {
                length--;
            }
            return new String(buffer, 0, length, "UTF-8").replace('\0', ' ');
        } catch (IOException e) {
            return "(" + e + ")";
        }
    }

    /**
     * The SHA-256 of `text` and the provider that gave it, through the JCA: libcore's Providers need Conscrypt's jar
     * from BOOTCLASSPATH (ADR-0046), else their initializer fails, which is printed instead.
     */
    private static String sha256(String text) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            StringBuilder hex = new StringBuilder();
            for (byte b : digest.digest(text.getBytes("UTF-8"))) {
                hex.append(String.format("%02x", b));
            }
            return digest.getProvider().getName() + " " + hex;
        } catch (Throwable t) {
            return "(" + t + ")";
        }
    }

    /** ActivityThread.currentProcessName, which is hidden: through reflection, as libraries reach it. */
    private static String currentProcessName() {
        try {
            return String.valueOf(Class.forName("android.app.ActivityThread").getMethod("currentProcessName")
                    .invoke(null));
        } catch (ReflectiveOperationException | RuntimeException e) {
            return "(" + e.getClass().getName() + ")";
        }
    }
}
