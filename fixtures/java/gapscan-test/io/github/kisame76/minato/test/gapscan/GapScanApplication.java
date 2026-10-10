package io.github.kisame76.minato.test.gapscan;

import android.app.Application;
import android.util.Log;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * The gap scanner's test, without a window. It scans a dex made on the spot, which refers to platform classes, methods
 * and fields that exist, that do not, that an app's class inherits, and to a type the dex defines itself, and then its
 * own APK, which must lack nothing. It logs under the tag gapscan-test what the scanner answers;
 * tools/android-reference.sh takes the log from Android 15, and tools/gapscan-check.sh compares Minato's with it.
 *
 * <p>To scan an APK, name its path in the environment variable GAPSCAN_APK or in the file gapscan-apk.txt next to the
 * app's files folder or in /data/local/tmp, one path per line, or as any .apk in the app's OBB folder (java_app_run.py
 * --obb): the answers go to the tag gapscan, which the test does not compare.
 */
public class GapScanApplication extends Application {
    private static final String TAG = "gapscan-test";

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            planted();
            own();
            targets();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    private static void show(String tag, String title, GapScan.Report report) {
        Log.i(tag, title + ": " + report.lines.size() + " lines");
        for (String line : report.lines) {
            Log.i(tag, line);
        }
    }

    /** A dex of references that are found, that are not, and that an app's class inherits. */
    private static void planted() {
        DexMaker dex = new DexMaker();
        int noArgs = dex.proto("I");
        int voidNoArgs = dex.proto("V");
        int chars = dex.proto("V", "[C");
        int date = dex.proto("V", "Ljava/util/Date;");
        int stream = dex.proto("Ljava/util/stream/Stream;");
        int unknownParameter = dex.proto("V", "Landroid/nosuch/Bar;");
        int ownMethod = dex.method("Lcom/example/Own;", voidNoArgs, "ownMethod");
        int ownField = dex.field("Lcom/example/Own;", "I", "ownField");
        // Found: declared, a constructor, inherited from a superclass and from an interface's default method, an
        // object method through an interface, a member of an app's class and one it inherits.
        dex.method("Ljava/lang/String;", noArgs, "length");
        dex.method("Ljava/lang/String;", chars, "<init>");
        dex.method("Ljava/util/ArrayList;", stream, "stream");
        dex.method("Ljava/util/List;", dex.proto("Z", "Ljava/lang/Object;"), "equals");
        dex.method("Ljava/util/ArrayList;", noArgs, "size");
        dex.method("Lcom/example/Own;", noArgs, "size");
        dex.field("Ljava/lang/Integer;", "I", "MAX_VALUE");
        dex.method("[Ljava/lang/String;", noArgs, "length");
        // Missing: a method, a constructor, one of an app's class that nothing above it has, a field, a class that is
        // not there with a member, and a method whose parameter is a class that is not there.
        dex.method("Ljava/lang/String;", voidNoArgs, "noSuchMethod");
        dex.method("Ljava/lang/String;", date, "<init>");
        dex.method("Lcom/example/Own;", voidNoArgs, "frobnicate");
        dex.field("Ljava/lang/Integer;", "I", "NOPE");
        dex.field("Ljava/lang/Integer;", "J", "MAX_VALUE");
        dex.method("Landroid/nosuch/Foo;", voidNoArgs, "bar");
        dex.method("Ljava/lang/String;", unknownParameter, "valueOf");
        // Not platform, or the dex's own: no gaps.
        dex.method("Lcom/example/Other;", voidNoArgs, "anything");
        int bundled = dex.method("Lorg/apache/http/Bundled;", voidNoArgs, "anything");
        dex.define("Lcom/example/Own;", "Ljava/util/ArrayList;", new String[0], new int[] {ownMethod},
                new int[] {ownField});
        dex.define("Lorg/apache/http/Bundled;", "Ljava/lang/Object;", new String[0], new int[] {bundled}, new int[0]);
        List<byte[]> dexes = new ArrayList<>();
        dexes.add(dex.build());
        show(TAG, "planted", GapScan.scan(dexes));
    }

    /** This APK, whose references the platform must all have. */
    private void own() throws Exception {
        GapScan.Report report = GapScan.scanApk(getPackageCodePath());
        Log.i(TAG, "own APK: " + report.classes + " missing classes, " + report.methods + " methods, "
                + report.fields + " fields");
        // A platform class whose members cannot all be read differs by device, so it is only shown.
        Log.i(TAG + "-device", "own APK: " + report.unreadable + " classes unreadable");
        for (String line : report.lines) {
            Log.i(TAG + "-device", line);
        }
    }

    private void targets() throws Exception {
        List<String> paths = new ArrayList<>();
        String env = System.getenv("GAPSCAN_APK");
        if (env != null && !env.isEmpty()) {
            paths.add(env);
        }
        for (File list : new File[] {new File(getFilesDir().getParentFile(), "gapscan-apk.txt"),
                new File("/data/local/tmp/gapscan-apk.txt")}) {
            if (list.isFile()) {
                for (String line : new String(Files.readAllBytes(list.toPath()), StandardCharsets.UTF_8).split("\n")) {
                    if (!line.trim().isEmpty()) {
                        paths.add(line.trim());
                    }
                }
            }
        }
        File[] inObb = getObbDir() == null ? null : getObbDir().listFiles((dir, name) -> name.endsWith(".apk"));
        if (inObb != null) {
            Arrays.sort(inObb);
            for (File apk : inObb) {
                paths.add(apk.getPath());
            }
        }
        for (String path : paths) {
            show("gapscan", path, GapScan.scanApk(path));
        }
    }
}
