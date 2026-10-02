package io.github.kisame76.minato.test.assetmanager;

import android.app.Application;
import android.content.res.ApkAssets;
import android.content.res.AssetManager;
import android.content.res.XmlResourceParser;
import android.os.SystemProperties;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * ADR-0039 b3.3's test: the framework's resource natives in libandroid_runtime.so, which app_process registers,
 * read framework-res and this APK below Resources (b3.4), from the Application class's static initializer as in
 * app-start-test. Each line goes to System.out with the prefix "asset-manager-test: "; Log.i and libandroidfw's
 * LOG(ERROR) go to logcat. The hidden calls are framework.jar's, which an app compiles against here.
 */
public class AssetManagerApplication extends Application {
    private static final String TAG = "asset-manager-test";

    static {
        try {
            report();
        } catch (Throwable e) {
            // A missing framework class is an Error: it fails the test with its name and the stack on stderr.
            print("FAILED " + e);
            e.printStackTrace();
        }
    }

    static void print(String line) {
        System.out.println(TAG + ": " + line);
    }

    static void report() throws IOException {
        // The system's assets: AssetManager's system path, with decision 8's framework-res folder.
        AssetManager system = AssetManager.getSystem();
        for (ApkAssets assets : system.getApkAssets()) {
            print("system ApkAssets " + assets.getDebugName());
        }
        listing(system, "");
        listing(system, "geoid_map");
        listing(system, "no-such-folder");
        asset(system, "geoid_map/README.md");
        layout(system, 1, "res/layout/simple_list_item_1.xml");
        String[] locales = system.getLocales();
        print("system locales include de " + Arrays.asList(locales).contains("de") + ", en-XA "
                + Arrays.asList(locales).contains("en-XA"));

        // This APK beside the system's, as an app's AssetManager holds them.
        ApkAssets app = ApkAssets.loadFromPath(apkPath());
        AssetManager assets = new AssetManager();
        assets.setApkAssets(new ApkAssets[] {app}, false);
        print("app ApkAssets " + assets.getApkAssets().length + ", the last " + app.getDebugName().endsWith("/base.apk"));
        listing(assets, "notes");
        asset(assets, "notes/hello.txt");

        print("ro.build.version.sdk " + SystemProperties.get("ro.build.version.sdk"));
        Log.i(TAG, "Log.i reaches logcat");
        // libandroidfw logs the missing folder with LOG(ERROR) before ApkAssets throws.
        try {
            ApkAssets.loadFromDir("/system/framework/no-such-folder", 0, null);
            print("loadFromDir of a missing folder loaded");
        } catch (IOException e) {
            print("loadFromDir of a missing folder throws " + e.getClass().getName());
        }
    }

    static void listing(AssetManager assets, String folder) throws IOException {
        String[] names = assets.list(folder);
        StringBuilder line = new StringBuilder("list \"" + folder + "\" " + names.length + ":");
        for (String name : names) {
            line.append(' ').append(name);
        }
        print(line.toString());
    }

    static void asset(AssetManager assets, String file) throws IOException {
        try (InputStream in = assets.open(file)) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            for (int read; (read = in.read(buffer)) > 0; ) {
                bytes.write(buffer, 0, read);
            }
            String text = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
            int end = text.indexOf('\n');
            print("open " + file + " " + bytes.size() + " bytes, \"" + (end < 0 ? text : text.substring(0, end)) + "\"");
        }
    }

    /** A compiled XML file of the cookie's ApkAssets: its elements and the first one's attribute count. */
    static void layout(AssetManager assets, int cookie, String file) throws IOException {
        StringBuilder elements = new StringBuilder();
        int attributes = -1;
        try (XmlResourceParser parser = assets.openXmlResourceParser(cookie, file)) {
            for (int event = parser.next(); event != XmlResourceParser.END_DOCUMENT; event = parser.next()) {
                if (event == XmlResourceParser.START_TAG) {
                    elements.append(' ').append(parser.getName());
                    if (attributes < 0) {
                        attributes = parser.getAttributeCount();
                    }
                }
            }
        } catch (org.xmlpull.v1.XmlPullParserException e) {
            throw new IOException(e);
        }
        print("xml " + file + " elements:" + elements + ", " + attributes + " attributes");
    }

    /** base.apk's path, from the class loader's URL of the manifest: jar:file:<path>!/AndroidManifest.xml. */
    static String apkPath() {
        URL manifest = AssetManagerApplication.class.getClassLoader().getResource("AndroidManifest.xml");
        String url = String.valueOf(manifest);
        return url.substring("jar:file:".length(), url.indexOf("!/"));
    }
}
