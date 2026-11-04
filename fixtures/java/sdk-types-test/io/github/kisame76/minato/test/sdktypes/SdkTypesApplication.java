package io.github.kisame76.minato.test.sdktypes;

import android.app.Application;
import android.graphics.ColorSpace;
import android.graphics.Picture;
import android.graphics.ParcelableColorSpace;
import android.graphics.drawable.PaintDrawable;
import android.graphics.drawable.PictureDrawable;
import android.hardware.GeomagneticField;
import android.provider.AlarmClock;
import android.provider.ContactsContract;
import android.provider.OpenableColumns;
import android.text.AlteredCharSequence;
import android.text.LoginFilter;
import android.text.util.Rfc822Token;
import android.util.AtomicFile;
import android.view.contentcapture.ContentCaptureManager;
import android.util.Log;
import android.util.MonthDisplayHelper;
import android.util.MutableBoolean;
import android.util.MutableByte;
import android.util.MutableChar;
import android.util.MutableDouble;
import android.util.MutableFloat;
import android.util.MutableInt;
import android.util.MutableLong;
import android.util.MutableShort;
import android.util.PrintStreamPrinter;
import android.util.PrintWriterPrinter;
import android.util.StringBuilderPrinter;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * The test of the plain framework types that SDKs and libraries reach, without a window (framework breadth, batch B4).
 * It logs under the tag sdk-types-test what the types answer: the mutable numbers and printers of android.util, an
 * AtomicFile's round trip, the Rfc822 token, the keystore exceptions' hierarchy, the provider constants,
 * GeomagneticField's numbers and the classes that only have to load. tools/android-reference.sh takes the log from
 * Android 15, and tools/sdk-types-check.sh compares Minato's with it.
 */
@SuppressWarnings("deprecation")
public class SdkTypesApplication extends Application {
    private static final String TAG = "sdk-types-test";

    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("mutables", SdkTypesApplication::mutables);
        part("printers", SdkTypesApplication::printers);
        part("atomicfile", this::atomicFile);
        part("rfc822", SdkTypesApplication::rfc822);
        part("provider", SdkTypesApplication::provider);
        part("services", this::services);
        part("keystore", SdkTypesApplication::keystore);
        part("geomagnetic", SdkTypesApplication::geomagnetic);
        part("text", SdkTypesApplication::text);
        part("graphics", SdkTypesApplication::graphics);
        part("month", SdkTypesApplication::month);
        part("classes", SdkTypesApplication::classes);
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    private static void mutables() {
        MutableInt i = new MutableInt(5);
        i.value += 2;
        MutableLong l = new MutableLong(1L << 40);
        MutableShort s = new MutableShort((short) 7);
        MutableByte b = new MutableByte((byte) -3);
        MutableChar c = new MutableChar('x');
        MutableFloat f = new MutableFloat(1.5f);
        MutableDouble d = new MutableDouble(2.25);
        MutableBoolean z = new MutableBoolean(false);
        z.value = !z.value;
        Log.i(TAG, "mutables: " + i.value + " " + l.value + " " + s.value + " " + b.value + " " + c.value + " " + f.value
                + " " + d.value + " " + z.value);
    }

    private static void printers() {
        StringBuilder text = new StringBuilder();
        new StringBuilderPrinter(text).println("one");
        StringWriter writer = new StringWriter();
        PrintWriter print = new PrintWriter(writer);
        new PrintWriterPrinter(print).println("two");
        print.flush();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        PrintStream stream = new PrintStream(bytes);
        new PrintStreamPrinter(stream).println("three");
        stream.flush();
        Log.i(TAG, "printers: " + text.toString().trim() + "|" + writer.toString().trim() + "|"
                + bytes.toString().trim());
    }

    private void atomicFile() throws IOException {
        File file = new File(getFilesDir(), "sdk-types-test.bin");
        AtomicFile atomic = new AtomicFile(file);
        FileOutputStream out = atomic.startWrite();
        out.write("atomic".getBytes(StandardCharsets.UTF_8));
        atomic.finishWrite(out);
        byte[] read = atomic.readFully();
        try (InputStream in = atomic.openRead()) {
            Log.i(TAG, "atomicfile: " + new String(read, StandardCharsets.UTF_8) + " " + in.read() + " "
                    + file.exists() + " " + atomic.getBaseFile().getName());
        }
        atomic.delete();
        Log.i(TAG, "atomicfile deleted: " + !file.exists());
    }

    private static void rfc822() {
        Rfc822Token token = new Rfc822Token("Ann Lee", "ann@example.com", "c");
        Log.i(TAG, "rfc822: " + token + " " + token.getName() + " " + token.getAddress() + " " + token.getComment());
        Log.i(TAG, "rfc822 quote: " + Rfc822Token.quoteName("A \"B\" C") + " "
                + Rfc822Token.quoteComment("x(y)") + " " + token.equals(new Rfc822Token("Ann Lee", "ann@example.com", "c")));
    }

    private static void provider() {
        Log.i(TAG, "provider: " + OpenableColumns.DISPLAY_NAME + " " + OpenableColumns.SIZE);
        Log.i(TAG, "provider alarm: " + AlarmClock.ACTION_SET_ALARM + " " + AlarmClock.EXTRA_HOUR + " "
                + AlarmClock.EXTRA_MINUTES + " " + AlarmClock.ACTION_SET_TIMER + " " + AlarmClock.EXTRA_LENGTH);
        // What Fresco's UriUtil reads in its static initializer (react-native's image view), and the contacts URIs
        // an app builds queries from.
        Log.i(TAG, "provider contacts: " + ContactsContract.AUTHORITY + " " + ContactsContract.AUTHORITY_URI + " "
                + ContactsContract.Contacts.CONTENT_URI + " " + ContactsContract.Contacts.CONTENT_LOOKUP_URI + " "
                + ContactsContract.Contacts.CONTENT_TYPE + " " + ContactsContract.RawContacts.CONTENT_URI + " "
                + ContactsContract.Data.CONTENT_URI + " " + ContactsContract.CommonDataKinds.Phone.CONTENT_URI + " "
                + ContactsContract.CommonDataKinds.Phone.NUMBER + " "
                + android.net.Uri.withAppendedPath(ContactsContract.AUTHORITY_URI, "display_photo"));
    }

    private void services() {
        // Content capture is an optional feature: Android names the service and hands an app that its provider has not
        // allowlisted no manager.
        Log.i(TAG, "services: " + getSystemServiceName(ContentCaptureManager.class) + " "
                + getSystemService(ContentCaptureManager.class));
    }

    private static void keystore() {
        String[] names = {"android.security.keystore.KeyPermanentlyInvalidatedException",
            "android.security.keystore.UserNotAuthenticatedException", "android.security.keystore.KeyExpiredException",
            "android.security.keystore.KeyNotYetValidException", "android.security.keystore.UserPresenceUnavailableException",
            "android.security.keystore.BackendBusyException", "android.security.KeyChainException"};
        for (String name : names) {
            try {
                Class<?> c = Class.forName(name);
                Throwable t = (Throwable) c.getConstructor(String.class).newInstance("m");
                Log.i(TAG, "keystore: " + c.getSimpleName() + " extends " + c.getSuperclass().getName() + " message "
                        + t.getMessage());
            } catch (ReflectiveOperationException e) {
                Log.i(TAG, "keystore: " + name + " failed: " + e);
            }
        }
        Log.i(TAG, "keystore classes: " + android.security.keystore.KeyInfo.class.getName() + " "
                + android.security.keystore.WrappedKeyEntry.class.getName() + " "
                + android.security.KeyPairGeneratorSpec.class.getName() + " "
                + android.security.KeyStoreParameter.class.getName() + " "
                + android.security.KeyChainAliasCallback.class.getName());
    }

    private static void geomagnetic() {
        GeomagneticField field = new GeomagneticField(48.1f, 11.6f, 520f, 1_700_000_000_000L);
        Log.i(TAG, String.format(Locale.US, "geomagnetic: %.3f %.3f %.1f %.1f", field.getDeclination(),
                field.getInclination(), field.getFieldStrength(), field.getHorizontalStrength()));
    }

    private static void text() {
        LoginFilter filter = new LoginFilter.UsernameFilterGMail();
        CharSequence filtered = filter.filter("A.b#c", 0, 5, new android.text.SpannableString(""), 0, 0);
        Log.i(TAG, "text: " + filtered);
        CharSequence altered = AlteredCharSequence.make("abcdef", new char[] {'X', 'Y'}, 1, 3);
        Log.i(TAG, "text altered: " + altered + " " + altered.length() + " " + altered.charAt(1));
    }

    private static void graphics() {
        PaintDrawable paint = new PaintDrawable(0xff336699);
        Log.i(TAG, "graphics: " + Integer.toHexString(paint.getPaint().getColor()) + " "
                + new PictureDrawable(new Picture()).getIntrinsicWidth());
        ParcelableColorSpace space = new ParcelableColorSpace(ColorSpace.get(ColorSpace.Named.SRGB));
        Log.i(TAG, "graphics colorspace: " + space.getColorSpace().getName());
    }

    private static void month() {
        MonthDisplayHelper helper = new MonthDisplayHelper(2024, 1);
        Log.i(TAG, "month: " + helper.getNumberOfDaysInMonth() + " " + helper.getFirstDayOfMonth() + " "
                + helper.getRowOf(15) + " " + helper.getDigitsForRow(2)[0] + " " + helper.getYear() + " "
                + helper.getMonth());
    }

    private static void classes() {
        String[] names = {"android.app.IntentService", "android.app.ListActivity", "android.app.ExpandableListActivity",
            "android.app.ListFragment", "android.app.AuthenticationRequiredException",
            "android.app.usage.ConfigurationStats", "android.app.usage.EventStats", "android.app.usage.StorageStats",
            "android.app.usage.ExternalStorageStats", "android.content.res.ObbInfo", "android.hardware.DataSpace",
            "android.speech.RecognizerIntent", "android.speech.RecognizerResultsIntent",
            
            
            "android.provider.FontsContract", "android.provider.UserDictionary",
            "android.provider.SearchRecentSuggestions", "android.util.TimingLogger", "android.util.Config",
            "android.util.LayoutDirection", "android.util.FloatMath"};
        for (String name : names) {
            try {
                Class<?> c = Class.forName(name, false, null);
                Log.i(TAG, "class " + name + ": " + c.getSuperclass().getName());
            } catch (Throwable t) {
                Log.i(TAG, "class " + name + ": failed " + t.getClass().getName());
            }
        }
    }
}
