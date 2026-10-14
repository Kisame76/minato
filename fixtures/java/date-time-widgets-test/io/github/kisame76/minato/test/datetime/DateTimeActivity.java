package io.github.kisame76.minato.test.datetime;

import android.app.Activity;
import android.app.DatePickerDialog;
import android.app.TimePickerDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.inputmethodservice.Keyboard;
import android.inputmethodservice.KeyboardView;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.widget.ArrayAdapter;
import android.widget.CalendarView;
import android.widget.DatePicker;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TimePicker;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.GregorianCalendar;
import java.util.TimeZone;

/**
 * The test of framework breadth batch F3: DatePicker, TimePicker, CalendarView, DatePickerDialog, TimePickerDialog,
 * Spinner, KeyboardView and the accessibility service they ask for, as an app uses them, in the window of the launcher
 * activity. It runs on the main thread when the window first has the focus, part after part with a pause for the
 * windows to settle, and logs under the tag date-time-widgets-test; tools/android-reference.sh takes the log from
 * Android 15, and tools/date-time-widgets-check.sh compares Minato's with it. Sizes and pixels depend on the screen
 * and are not logged: a widget is checked for having been measured, laid out and drawn.
 */
public class DateTimeActivity extends Activity {
    static final String TAG = "date-time-widgets-test";
    private static final int SETTLE_MS = 700;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            StringBuilder line = new StringBuilder(name);
            for (Throwable c = t; c != null; c = c.getCause()) {
                line.append(": ").append(c.getClass().getName()).append(": ").append(c.getMessage());
            }
            log(line.toString());
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        final Runnable[] steps = {
            () -> part("datePickerCalendar", this::datePickerCalendar),
            () -> part("datePickerSpinner", this::datePickerSpinner),
            () -> part("timePickerClock", this::timePickerClock),
            () -> part("timePickerSpinner", this::timePickerSpinner),
            () -> part("calendarView", this::calendarView),
            () -> part("spinner", this::spinner),
            () -> part("keyboardView", this::keyboardView),
            () -> part("accessibility", this::accessibility),
            () -> part("datePickerDialog", this::datePickerDialog),
            () -> part("timePickerDialog", this::timePickerDialog),
            () -> {
                log("done");
                finish();
            },
        };
        final int[] index = {0};
        Runnable next = new Runnable() {
            @Override
            public void run() {
                steps[index[0]++].run();
                if (index[0] < steps.length) {
                    mHandler.postDelayed(this, SETTLE_MS);
                }
            }
        };
        mHandler.post(next);
    }

    /** Puts the view in the window, lays it out at a fixed width, draws it and says whether anything was drawn. */
    private String shown(View view) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.addView(view, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(root);
        root.measure(View.MeasureSpec.makeMeasureSpec(1080, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(2400, View.MeasureSpec.AT_MOST));
        root.layout(0, 0, root.getMeasuredWidth(), root.getMeasuredHeight());
        Bitmap bitmap = Bitmap.createBitmap(root.getWidth(), Math.max(1, root.getHeight()), Bitmap.Config.ARGB_8888);
        root.draw(new Canvas(bitmap));
        int first = bitmap.getPixel(0, 0);
        boolean varied = false;
        for (int y = 0; y < bitmap.getHeight() && !varied; y += 7) {
            for (int x = 0; x < bitmap.getWidth(); x += 7) {
                if (bitmap.getPixel(x, y) != first) {
                    varied = true;
                    break;
                }
            }
        }
        return "measured=" + (view.getMeasuredHeight() > 0) + " laidOut=" + (view.getHeight() > 0) + " drawn=" + varied;
    }

    private static long time(int year, int month, int day) {
        GregorianCalendar calendar = new GregorianCalendar(TimeZone.getTimeZone("UTC"));
        calendar.clear();
        calendar.set(year, month, day, 12, 0, 0);
        return calendar.getTimeInMillis();
    }

    private void datePickerCalendar() {
        DatePicker picker = new DatePicker(this);
        final ArrayList<String> heard = new ArrayList<>();
        picker.init(2024, Calendar.FEBRUARY, 29, (view, year, month, day) -> heard.add(year + "-" + month + "-" + day));
        log("datePicker calendar: " + picker.getYear() + "-" + picker.getMonth() + "-" + picker.getDayOfMonth()
                + " spinnersShown=" + picker.getSpinnersShown() + " calendarViewShown=" + picker.getCalendarViewShown()
                + " enabled=" + picker.isEnabled());
        picker.updateDate(2025, Calendar.DECEMBER, 31);
        log("datePicker updated: " + picker.getYear() + "-" + picker.getMonth() + "-" + picker.getDayOfMonth()
                + " heard=" + heard);
        picker.setMinDate(time(2025, Calendar.JANUARY, 1));
        picker.setMaxDate(time(2025, Calendar.DECEMBER, 31));
        log("datePicker bounds: min=" + picker.getMinDate() + " max=" + picker.getMaxDate());
        picker.updateDate(2030, Calendar.JANUARY, 15);
        log("datePicker clamped: " + picker.getYear() + "-" + picker.getMonth() + "-" + picker.getDayOfMonth());
        picker.setFirstDayOfWeek(Calendar.MONDAY);
        log("datePicker first day: " + picker.getFirstDayOfWeek());
        log("datePicker " + shown(picker));
    }

    private void datePickerSpinner() {
        DatePicker picker = new DatePicker(this, null, 0, android.R.style.Widget_Holo_DatePicker);
        final ArrayList<String> heard = new ArrayList<>();
        picker.init(2024, Calendar.FEBRUARY, 29, (view, year, month, day) -> heard.add(year + "-" + month + "-" + day));
        log("datePicker spinner: " + picker.getYear() + "-" + picker.getMonth() + "-" + picker.getDayOfMonth()
                + " spinnersShown=" + picker.getSpinnersShown()
                + " calendarViewShown=" + picker.getCalendarViewShown());
        picker.updateDate(2023, Calendar.MARCH, 1);
        log("datePicker spinner updated: " + picker.getYear() + "-" + picker.getMonth() + "-" + picker.getDayOfMonth()
                + " heard=" + heard);
        picker.setSpinnersShown(false);
        log("datePicker spinner hidden: " + picker.getSpinnersShown());
        picker.setSpinnersShown(true);
        log("datePicker spinner " + shown(picker));
    }

    private void timePickerClock() {
        TimePicker picker = new TimePicker(this);
        final ArrayList<String> heard = new ArrayList<>();
        picker.setOnTimeChangedListener((view, hour, minute) -> heard.add(hour + ":" + minute));
        picker.setIs24HourView(true);
        picker.setHour(13);
        picker.setMinute(45);
        log("timePicker clock: " + picker.getHour() + ":" + picker.getMinute() + " is24=" + picker.is24HourView()
                + " last=" + heard.get(heard.size() - 1));
        picker.setIs24HourView(false);
        picker.setHour(7);
        log("timePicker clock 12h: " + picker.getHour() + ":" + picker.getMinute() + " is24=" + picker.is24HourView());
        log("timePicker clock " + shown(picker));
    }

    private void timePickerSpinner() {
        // Not public API: the spinner style of TimePicker, by name.
        int style = getResources().getIdentifier("Widget.Holo.TimePicker", "style", "android");
        TimePicker picker = new TimePicker(this, null, 0, style);
        final ArrayList<String> heard = new ArrayList<>();
        picker.setOnTimeChangedListener((view, hour, minute) -> heard.add(hour + ":" + minute));
        picker.setIs24HourView(true);
        picker.setHour(23);
        picker.setMinute(59);
        log("timePicker spinner: " + picker.getHour() + ":" + picker.getMinute() + " is24=" + picker.is24HourView()
                + " last=" + heard.get(heard.size() - 1));
        log("timePicker spinner " + shown(picker));
    }

    private void calendarView() {
        CalendarView view = new CalendarView(this);
        final ArrayList<String> heard = new ArrayList<>();
        view.setOnDateChangeListener((v, year, month, day) -> heard.add(year + "-" + month + "-" + day));
        long date = time(2024, Calendar.FEBRUARY, 29);
        view.setDate(date);
        log("calendarView: date=" + view.getDate() + " same=" + (view.getDate() == date) + " first="
                + view.getFirstDayOfWeek() + " heard=" + heard);
        view.setMinDate(time(2024, Calendar.JANUARY, 1));
        view.setMaxDate(time(2024, Calendar.DECEMBER, 31));
        log("calendarView bounds: " + (view.getMinDate() == time(2024, Calendar.JANUARY, 1)) + " "
                + (view.getMaxDate() == time(2024, Calendar.DECEMBER, 31)));
        view.setFirstDayOfWeek(Calendar.MONDAY);
        log("calendarView first day: " + view.getFirstDayOfWeek());
        log("calendarView " + shown(view));
    }

    private void spinner() {
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item,
                new String[] {"one", "two", "three"});
        final ArrayList<String> heard = new ArrayList<>();
        spinner.setAdapter(adapter);
        spinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                heard.add(position + ":" + parent.getItemAtPosition(position));
            }

            @Override
            public void onNothingSelected(android.widget.AdapterView<?> parent) {
                heard.add("none");
            }
        });
        spinner.setSelection(2);
        log("spinner: selected=" + spinner.getSelectedItemPosition() + " item=" + spinner.getSelectedItem()
                + " count=" + spinner.getCount());
        log("spinner " + shown(spinner));
        log("spinner heard: " + heard);
    }

    private void keyboardView() {
        KeyboardView view = new KeyboardView(this, null);
        final ArrayList<String> heard = new ArrayList<>();
        view.setOnKeyboardActionListener(new KeyboardView.OnKeyboardActionListener() {
            @Override
            public void onPress(int primaryCode) {
                heard.add("press " + primaryCode);
            }

            @Override
            public void onRelease(int primaryCode) {
                heard.add("release " + primaryCode);
            }

            @Override
            public void onKey(int primaryCode, int[] keyCodes) {
                heard.add("key " + primaryCode);
            }

            @Override
            public void onText(CharSequence text) {
                heard.add("text " + text);
            }

            @Override
            public void swipeLeft() {
            }

            @Override
            public void swipeRight() {
            }

            @Override
            public void swipeDown() {
            }

            @Override
            public void swipeUp() {
            }
        });
        Keyboard keyboard = new Keyboard(this, getResources().getIdentifier("letters", "xml", getPackageName()));
        view.setKeyboard(keyboard);
        StringBuilder keys = new StringBuilder();
        for (Keyboard.Key key : keyboard.getKeys()) {
            keys.append(key.label).append('=').append(key.codes[0]).append(key.repeatable ? "r " : " ");
        }
        log("keyboardView: keys=" + keyboard.getKeys().size() + " " + keys.toString().trim() + " shifted="
                + view.isShifted() + " preview=" + view.isPreviewEnabled());
        log("keyboardView " + shown(view));
        Keyboard.Key first = keyboard.getKeys().get(1);
        float x = first.x + first.width / 2f + view.getPaddingLeft();
        float y = first.y + first.height / 2f + view.getPaddingTop();
        long now = android.os.SystemClock.uptimeMillis();
        view.dispatchTouchEvent(MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN, x, y, 0));
        view.dispatchTouchEvent(MotionEvent.obtain(now, now + 20, MotionEvent.ACTION_UP, x, y, 0));
        log("keyboardView touched: " + heard);
        view.closing();
    }

    /** What an app, AndroidX and the date and time widgets ask the accessibility service when none is enabled. */
    private void accessibility() {
        AccessibilityManager manager = (AccessibilityManager) getSystemService(Context.ACCESSIBILITY_SERVICE);
        log("accessibility: service=" + (manager != null) + " same="
                + (manager == AccessibilityManager.getInstance(this)) + " enabled=" + manager.isEnabled()
                + " touchExploration=" + manager.isTouchExplorationEnabled());
        AccessibilityManager.AccessibilityStateChangeListener state = enabled -> { };
        AccessibilityManager.TouchExplorationStateChangeListener touch = enabled -> { };
        log("accessibility state listener: add=" + manager.addAccessibilityStateChangeListener(state) + " addAgain="
                + manager.addAccessibilityStateChangeListener(state) + " remove="
                + manager.removeAccessibilityStateChangeListener(state) + " removeAgain="
                + manager.removeAccessibilityStateChangeListener(state));
        log("accessibility touch listener: add=" + manager.addTouchExplorationStateChangeListener(touch) + " addAgain="
                + manager.addTouchExplorationStateChangeListener(touch) + " remove="
                + manager.removeTouchExplorationStateChangeListener(touch) + " removeAgain="
                + manager.removeTouchExplorationStateChangeListener(touch));
        AccessibilityManager.AccessibilityServicesStateChangeListener services = m -> { };
        manager.addAccessibilityServicesStateChangeListener(services);
        log("accessibility services listener: remove="
                + manager.removeAccessibilityServicesStateChangeListener(services) + " removeAgain="
                + manager.removeAccessibilityServicesStateChangeListener(services));
        AccessibilityManager.AudioDescriptionRequestedChangeListener audio = requested -> { };
        manager.addAudioDescriptionRequestedChangeListener(getMainExecutor(), audio);
        log("accessibility audio description: requested=" + manager.isAudioDescriptionRequested() + " remove="
                + manager.removeAudioDescriptionRequestedChangeListener(audio) + " removeAgain="
                + manager.removeAudioDescriptionRequestedChangeListener(audio));
        log("accessibility lists: enabled="
                + manager.getEnabledAccessibilityServiceList(android.accessibilityservice.AccessibilityServiceInfo
                        .FEEDBACK_ALL_MASK).size()
                + " enabledSpoken=" + manager.getEnabledAccessibilityServiceList(
                        android.accessibilityservice.AccessibilityServiceInfo.FEEDBACK_SPOKEN).size()
                + " installedIsList=" + (manager.getInstalledAccessibilityServiceList() != null)
                + " serviceInfosIsList=" + (manager.getAccessibilityServiceList() != null));
        log("accessibility timeout: " + manager.getRecommendedTimeoutMillis(1000,
                AccessibilityManager.FLAG_CONTENT_TEXT) + " " + manager.getRecommendedTimeoutMillis(0,
                AccessibilityManager.FLAG_CONTENT_CONTROLS | AccessibilityManager.FLAG_CONTENT_ICONS) + " "
                + manager.getRecommendedTimeoutMillis(2500, 0));
        log("accessibility misc: button=" + AccessibilityManager.isAccessibilityButtonSupported() + " fromTool="
                + manager.isRequestFromAccessibilityTool() + " strokeWidthSet="
                + (manager.getAccessibilityFocusStrokeWidth() > 0) + " colorSet="
                + (manager.getAccessibilityFocusColor() != 0));
        AccessibilityEvent event = AccessibilityEvent.obtain(AccessibilityEvent.TYPE_VIEW_CLICKED);
        try {
            manager.sendAccessibilityEvent(event);
            log("accessibility event: sent");
        } catch (RuntimeException e) {
            log("accessibility event: " + e.getClass().getName() + ": " + e.getMessage());
        }
        // Off the main thread accessibility may have changed meanwhile: AOSP logs and returns instead of throwing.
        final String[] worker = new String[1];
        Thread thread = new Thread(() -> {
            try {
                manager.sendAccessibilityEvent(AccessibilityEvent.obtain(AccessibilityEvent.TYPE_VIEW_CLICKED));
                manager.interrupt();
                worker[0] = "no exception";
            } catch (Throwable t) {
                worker[0] = t.getClass().getName() + ": " + t.getMessage();
            }
        });
        thread.start();
        try {
            thread.join();
        } catch (InterruptedException e) {
            worker[0] = "interrupted";
        }
        log("accessibility worker thread: " + worker[0]);
        try {
            manager.interrupt();
            log("accessibility interrupt: done");
        } catch (RuntimeException e) {
            log("accessibility interrupt: " + e.getClass().getName() + ": " + e.getMessage());
        }
        View view = new View(this);
        view.sendAccessibilityEvent(AccessibilityEvent.TYPE_VIEW_CLICKED);
        view.announceForAccessibility("announced");
        log("accessibility view: important=" + view.isImportantForAccessibility() + " focused="
                + view.isAccessibilityFocused() + " live=" + view.getAccessibilityLiveRegion());
    }

    private void datePickerDialog() {
        final ArrayList<String> heard = new ArrayList<>();
        DatePickerDialog dialog = new DatePickerDialog(this, (view, year, month, day) -> heard.add(
                year + "-" + month + "-" + day), 2024, Calendar.FEBRUARY, 29);
        dialog.show();
        DatePicker picker = dialog.getDatePicker();
        log("datePickerDialog: showing=" + dialog.isShowing() + " " + picker.getYear() + "-" + picker.getMonth() + "-"
                + picker.getDayOfMonth() + " positive=" + (dialog.getButton(DialogInterface.BUTTON_POSITIVE) != null)
                + " negative=" + (dialog.getButton(DialogInterface.BUTTON_NEGATIVE) != null));
        dialog.updateDate(2025, Calendar.JULY, 4);
        Bundle state = dialog.onSaveInstanceState();
        log("datePickerDialog state: " + state.getInt("year") + "-" + state.getInt("month") + "-"
                + state.getInt("day"));
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick();
        // The dialog answers through its button handler, a message on the main thread: the answer is logged later.
        mHandler.postDelayed(() -> log("datePickerDialog picked: " + heard + " showing=" + dialog.isShowing()), 300);
    }

    private void timePickerDialog() {
        final ArrayList<String> heard = new ArrayList<>();
        TimePickerDialog dialog = new TimePickerDialog(this, (view, hour, minute) -> heard.add(hour + ":" + minute),
                13, 45, true);
        dialog.show();
        log("timePickerDialog: showing=" + dialog.isShowing() + " positive="
                + (dialog.getButton(DialogInterface.BUTTON_POSITIVE) != null) + " negative="
                + (dialog.getButton(DialogInterface.BUTTON_NEGATIVE) != null));
        dialog.updateTime(6, 5);
        Bundle state = dialog.onSaveInstanceState();
        log("timePickerDialog state: " + state.getInt("hour") + ":" + state.getInt("minute") + " is24="
                + state.getBoolean("is24hour"));
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick();
        mHandler.postDelayed(() -> log("timePickerDialog picked: " + heard + " showing=" + dialog.isShowing()), 300);
    }
}
