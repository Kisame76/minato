package io.github.kisame76.minato.test.resources;

import android.app.Application;
import android.content.res.ApkAssets;
import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XmlResourceParser;
import android.os.LocaleList;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * ADR-0039 b3.4's test: Resources, ResourcesImpl and Theme over libandroid_runtime.so's natives read the system's
 * resources (framework-res's folder) and this APK's, from the Application class's static initializer as in
 * app-start-test, before any Context exists. The app's Resources are built as ResourcesManager builds an app's: its
 * ApkAssets beside the system's, with decision 9's density 480. Each line goes to System.out with the prefix
 * "resources-test: ".
 */
public class ResourcesApplication extends Application {
    private static final String PACKAGE = "io.github.kisame76.minato.test.resources";

    static {
        try {
            report();
        } catch (Throwable e) {
            // A missing framework class is an Error: it fails the test with its name and the stack on stderr.
            print("FAILED " + e);
            e.printStackTrace();
        }
        // The last line, after which the run on the Mac ends (framework/tests/java_app_run.py, step c2b).
        print("done");
    }

    static void print(String line) {
        System.out.println("resources-test: " + line);
    }

    // Resources(AssetManager, DisplayMetrics, Configuration) and updateConfiguration are deprecated for apps, and
    // ResourcesManager still builds Resources this way.
    @SuppressWarnings("deprecation")
    static void report() throws IOException {
        Resources system = Resources.getSystem();
        print("system cancel \"" + system.getString(android.R.string.cancel) + "\"");

        AssetManager assets = new AssetManager();
        assets.setApkAssets(new ApkAssets[] {ApkAssets.loadFromPath(apkPath())}, false);
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        metrics.densityDpi = 480;
        metrics.density = 3f;
        metrics.scaledDensity = 3f;
        Configuration config = new Configuration();
        config.setToDefaults();
        config.densityDpi = 480;
        config.setLocales(LocaleList.forLanguageTags("en-US"));
        Resources res = new Resources(assets, metrics, config);

        print("app_name (en-US) \"" + res.getString(id(res, "string", "app_name")) + "\"");
        print("color harbor #" + Integer.toHexString(res.getColor(id(res, "color", "harbor"), null)));
        print("integer answer " + res.getInteger(id(res, "integer", "answer")));
        print("dimen gap " + res.getDimension(id(res, "dimen", "gap")) + " px, "
                + res.getDimensionPixelSize(id(res, "dimen", "gap")) + " px whole");
        print("dimen text " + res.getDimension(id(res, "dimen", "text")) + " px");
        print("plurals apps " + res.getQuantityString(id(res, "plurals", "apps"), 1, 1) + ", "
                + res.getQuantityString(id(res, "plurals", "apps"), 5, 5));
        try (InputStream in = res.openRawResource(id(res, "raw", "data"))) {
            print("raw data \"" + firstLine(in) + "\"");
        }
        try (AssetFileDescriptor fd = res.openRawResourceFd(id(res, "raw", "data"))) {
            print("raw data as a file descriptor " + fd.getLength() + " bytes");
        }
        StringBuilder elements = new StringBuilder();
        try (XmlResourceParser parser = res.getLayout(id(res, "layout", "main"))) {
            for (int event = parser.next(); event != XmlResourceParser.END_DOCUMENT; event = parser.next()) {
                if (event == XmlResourceParser.START_TAG) {
                    elements.append(' ').append(parser.getName());
                }
            }
        } catch (org.xmlpull.v1.XmlPullParserException e) {
            throw new IOException(e);
        }
        print("layout main elements:" + elements);

        // TextAppearance.Medium through a theme, as a view's obtainStyledAttributes reads a text appearance.
        Resources.Theme theme = res.newTheme();
        TypedArray appearance = theme.obtainStyledAttributes(android.R.style.TextAppearance_Medium,
                new int[] {android.R.attr.textSize, android.R.attr.textStyle});
        TypedValue size = appearance.peekValue(0);
        print("TextAppearance.Medium textSize " + TypedValue.coerceToString(size.type, size.data) + ", "
                + appearance.getDimension(0, -1) + " px, textStyle " + appearance.getInt(1, -1));
        appearance.recycle();

        config.setLocales(LocaleList.forLanguageTags("de-DE"));
        res.updateConfiguration(config, metrics);
        print("app_name (de-DE) \"" + res.getString(id(res, "string", "app_name")) + "\"");
        print("greeting (de-DE) \"" + res.getString(id(res, "string", "greeting"), "Minato") + "\"");
        print("system cancel through the app (de-DE) \"" + res.getString(android.R.string.cancel) + "\"");
    }

    static int id(Resources res, String type, String name) {
        return res.getIdentifier(name, type, PACKAGE);
    }

    static String firstLine(InputStream in) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        for (int read; (read = in.read(buffer)) > 0; ) {
            bytes.write(buffer, 0, read);
        }
        String text = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
        int end = text.indexOf('\n');
        return end < 0 ? text : text.substring(0, end);
    }

    /** base.apk's path, from the class loader's URL of the manifest: jar:file:<path>!/AndroidManifest.xml. */
    static String apkPath() {
        URL manifest = ResourcesApplication.class.getClassLoader().getResource("AndroidManifest.xml");
        String url = String.valueOf(manifest);
        return url.substring("jar:file:".length(), url.indexOf("!/"));
    }
}
