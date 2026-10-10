package io.github.kisame76.minato.test.timezone;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZonedDateTime;
import java.util.Arrays;
import java.util.Calendar;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * The compatibility plan's gap 21 test: the time zone data an app reads. Its parts run one after the other from the
 * resumed activity: the tzdata module's files as Android 15 lays them out, its versioned folder with ICU's files among
 * them; java.util.TimeZone's rules, names and IDs, which libcore reads from the module's versioned tzdata; the date
 * formats and Calendar over them; java.time's zones and ICU's; each for two fixed instants, in zones with and without
 * daylight saving time. Each part logs with the tag timezone-test. What differs by device or by the module's data
 * version (sizes, versions, counts, the device's zone) has the tag timezone-test-device. A part that throws logs
 * "failed" and the next one runs, so that every run reaches "done".
 */
public class TimezoneActivity extends Activity {
    static final String TAG = "timezone-test";
    static final String DEVICE = TAG + "-device";
    static final String MODULE = "/apex/com.android.tzdata/etc/tz/";
    /** 2024-01-15T12:00:00Z and 2024-07-15T12:00:00Z: winter and summer in the north. */
    static final long JANUARY = 1705320000000L;
    static final long JULY = 1721044800000L;
    static final String[] ZONES = {"America/New_York", "Europe/Berlin", "Asia/Kolkata", "Australia/Lord_Howe",
            "America/Sao_Paulo", "Pacific/Chatham"};

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private interface Part {
        void run() throws Exception;
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mStarted) {
            mStarted = true;
            mHandler.post(this::runParts);
        }
    }

    private void runParts() {
        List<Part> parts = Arrays.asList(this::files, this::zones, this::names, this::ids, this::formats,
                this::javaTime, this::icu, this::deviceZone);
        for (Part part : parts) {
            try {
                part.run();
            } catch (Exception | LinkageError e) {
                Log.i(TAG, "failed: " + e.getClass().getName() + ": " + e.getMessage());
                Log.i(DEVICE, Log.getStackTraceString(e));
            }
        }
        Log.i(TAG, "done");
    }

    /** The module's files that libcore, ICU and bionic read, whether each is there; sizes and version by device. */
    private void files() throws IOException {
        String[] names = {"tzdata", "tz_version", "versioned/8/tzdata", "versioned/8/tz_version",
                "versioned/8/tzlookup.xml", "versioned/8/telephonylookup.xml", "versioned/8/icu/zoneinfo64.res",
                "versioned/8/icu/metaZones.res", "versioned/8/icu/timezoneTypes.res",
                "versioned/8/icu/windowsZones.res"};
        for (String name : names) {
            File file = new File(MODULE + name);
            Log.i(TAG, "file " + name + ": " + file.isFile());
            if (file.isFile()) Log.i(DEVICE, "file " + name + ": " + file.length() + " bytes");
        }
        File version = new File(MODULE + "versioned/8/tz_version");
        if (version.isFile()) {
            byte[] bytes = Files.readAllBytes(version.toPath());
            Log.i(DEVICE, "tz_version: " + new String(bytes, StandardCharsets.US_ASCII).trim());
        }
        for (String folder : new String[] {"", "versioned/8/", "versioned/8/icu/"}) {
            String[] list = new File(MODULE + folder).list();
            if (list != null) Arrays.sort(list);
            Log.i(DEVICE, "folder tz/" + folder + ": " + (list == null ? "not listed" : String.join(" ", list)));
        }
    }

    /** java.util.TimeZone's rules for each zone: libcore's ZoneInfo, from the module's versioned tzdata. */
    private void zones() {
        for (String id : ZONES) {
            TimeZone zone = TimeZone.getTimeZone(id);
            Log.i(TAG, id + ": id " + zone.getID() + ", raw " + zone.getRawOffset() + ", january "
                    + zone.getOffset(JANUARY) + ", july " + zone.getOffset(JULY) + ", daylight "
                    + zone.useDaylightTime() + " " + zone.getDSTSavings() + ", in daylight in july "
                    + zone.inDaylightTime(new Date(JULY)));
        }
        Log.i(TAG, "unknown: " + TimeZone.getTimeZone("Mars/Olympus").getID());
        TimeZone custom = TimeZone.getTimeZone("GMT+05:30");
        Log.i(TAG, "custom: " + custom.getID() + ", raw " + custom.getRawOffset());
    }

    /** Zone names, which libcore takes from ICU for the zone it found. */
    private void names() {
        TimeZone york = TimeZone.getTimeZone("America/New_York");
        TimeZone berlin = TimeZone.getTimeZone("Europe/Berlin");
        Log.i(TAG, "names: " + york.getDisplayName(false, TimeZone.LONG, Locale.US) + ", "
                + york.getDisplayName(true, TimeZone.SHORT, Locale.US) + ", "
                + berlin.getDisplayName(true, TimeZone.LONG, Locale.GERMANY));
    }

    /** The zone IDs that java.util.TimeZone knows; their count by device. */
    private void ids() {
        List<String> ids = Arrays.asList(TimeZone.getAvailableIDs());
        Log.i(DEVICE, "ids: " + ids.size());
        Log.i(TAG, "ids: more than 400 " + (ids.size() > 400) + ", Europe/Berlin " + ids.contains("Europe/Berlin"));
        List<String> plusOne = Arrays.asList(TimeZone.getAvailableIDs(3600000));
        Log.i(TAG, "ids at +01:00: Europe/Berlin " + plusOne.contains("Europe/Berlin") + ", Asia/Kolkata "
                + plusOne.contains("Asia/Kolkata"));
    }

    /** SimpleDateFormat and Calendar in a zone. */
    private void formats() {
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd HH:mm zzz", Locale.US);
        format.setTimeZone(TimeZone.getTimeZone("America/New_York"));
        Log.i(TAG, "format: " + format.format(new Date(JANUARY)) + ", " + format.format(new Date(JULY)));
        Calendar calendar = Calendar.getInstance(TimeZone.getTimeZone("Asia/Kolkata"), Locale.US);
        calendar.setTimeInMillis(JANUARY);
        Log.i(TAG, "calendar: " + calendar.get(Calendar.HOUR_OF_DAY) + ":" + calendar.get(Calendar.MINUTE)
                + ", zone offset " + calendar.get(Calendar.ZONE_OFFSET));
    }

    /** java.time's zones, whose rules Android takes from ICU. */
    private void javaTime() {
        for (String id : new String[] {"America/New_York", "Pacific/Chatham"}) {
            Log.i(TAG, "java.time: " + ZonedDateTime.ofInstant(Instant.ofEpochMilli(JULY), ZoneId.of(id)));
        }
        Log.i(TAG, "java.time ids: more than 400 " + (ZoneId.getAvailableZoneIds().size() > 400));
    }

    /** ICU's zones, from the module's files where they are and from ICU's own data otherwise; its version by device. */
    private void icu() {
        android.icu.util.TimeZone zone = android.icu.util.TimeZone.getTimeZone("America/New_York");
        Log.i(TAG, "icu: " + zone.getID() + ", january " + zone.getOffset(JANUARY) + ", july " + zone.getOffset(JULY));
        Log.i(DEVICE, "icu data version: " + android.icu.util.TimeZone.getTZDataVersion());
    }

    /** The device's zone, which Android gives apps through persist.sys.timezone. */
    private void deviceZone() {
        Log.i(DEVICE, "device zone: " + TimeZone.getDefault().getID() + ", java.time " + ZoneId.systemDefault()
                + ", icu " + android.icu.util.TimeZone.getDefault().getID());
    }
}
