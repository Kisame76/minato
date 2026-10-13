package io.github.kisame76.minato.test.preferences;

import android.app.Application;
import android.content.SharedPreferences;
import android.preference.CheckBoxPreference;
import android.preference.EditTextPreference;
import android.preference.ListPreference;
import android.preference.MultiSelectListPreference;
import android.preference.Preference;
import android.preference.PreferenceCategory;
import android.preference.PreferenceManager;
import android.preference.SwitchPreference;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/**
 * The test of android.preference, without a window (framework breadth, step (c)). It logs under the tag
 * preferences-test the defaults that setDefaultValues writes from an XML tree of every preference type into the
 * default SharedPreferences (type and value of each entry, sorted), then the state and arithmetic of the types
 * themselves: checked state and summaries, list entries and values, multi-select sets, ordering and categories.
 * Public API only. tools/android-reference.sh takes the log from Android 15, and tools/preferences-check.sh compares
 * Minato's with it.
 */
public class PreferencesApplication extends Application {
    private static final String TAG = "preferences-test";

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            defaults();
        } catch (Throwable t) {
            Log.i(TAG, "defaults failed: " + t.getClass().getName());
        }
        probe("checkbox", this::checkBox);
        probe("switch", this::switchPreference);
        probe("edittext", this::editText);
        probe("list", this::list);
        probe("multi", this::multi);
        probe("order", this::order);
        probe("category", this::category);
        probe("plain", this::plain);
        Log.i(TAG, "done");
    }

    private interface Body {
        void run() throws Exception;
    }

    private void probe(String name, Body body) {
        try {
            body.run();
        } catch (Throwable t) {
            Log.i(TAG, name + " failed: " + t.getClass().getName());
        }
    }

    private void defaults() {
        int xml = getResources().getIdentifier("preferences", "xml", getPackageName());
        PreferenceManager.setDefaultValues(this, xml, false);
        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(this);
        Map<String, ?> all = prefs.getAll();
        List<String> keys = new ArrayList<>(all.keySet());
        Collections.sort(keys);
        for (String key : keys) {
            Object value = all.get(key);
            String text = value instanceof Set ? new TreeSet<Object>((Set<?>) value).toString() : String.valueOf(value);
            Log.i(TAG, "default " + key + " " + value.getClass().getSimpleName().replaceAll("Linked|Hash|Tree", "")
                    + " " + text);
        }
        // A second call with readAgain=false keeps what the first wrote, even after a change.
        prefs.edit().putBoolean("sound", false).apply();
        PreferenceManager.setDefaultValues(this, xml, false);
        Log.i(TAG, "default again sound " + prefs.getBoolean("sound", true));
        PreferenceManager.setDefaultValues(this, xml, true);
        Log.i(TAG, "default read again sound " + prefs.getBoolean("sound", false));
        Log.i(TAG, "name " + PreferenceManager.getDefaultSharedPreferencesName(this));
    }

    private void checkBox() {
        CheckBoxPreference pref = new CheckBoxPreference(this);
        pref.setKey("k");
        pref.setTitle("T");
        pref.setSummaryOn("on");
        pref.setSummaryOff("off");
        Log.i(TAG, "checkbox " + pref.isChecked() + " " + pref.getSummaryOn() + " " + pref.getSummaryOff()
                + " dis=" + pref.shouldDisableDependents());
        pref.setChecked(true);
        Log.i(TAG, "checkbox " + pref.isChecked() + " dis=" + pref.shouldDisableDependents() + " "
                + pref.getKey() + " " + pref.getTitle());
        pref.setDisableDependentsState(true);
        Log.i(TAG, "checkbox inverted dis=" + pref.shouldDisableDependents() + " "
                + pref.getDisableDependentsState());
        pref.setDefaultValue(true);
        Log.i(TAG, "checkbox default " + pref.isEnabled() + " " + pref.isSelectable() + " " + pref.isPersistent());
    }

    private void switchPreference() {
        SwitchPreference pref = new SwitchPreference(this);
        pref.setSwitchTextOn("Yes");
        pref.setSwitchTextOff("No");
        pref.setSummaryOn("enabled");
        pref.setSummaryOff("disabled");
        Log.i(TAG, "switch " + pref.isChecked() + " " + pref.getSwitchTextOn() + " " + pref.getSwitchTextOff() + " "
                + pref.getSummaryOn() + " " + pref.getSummaryOff());
        pref.setChecked(true);
        Log.i(TAG, "switch " + pref.isChecked());
    }

    private void editText() {
        EditTextPreference pref = new EditTextPreference(this);
        pref.setKey("name");
        pref.setDialogTitle("Dialog");
        Log.i(TAG, "edittext " + pref.getText() + " " + pref.getDialogTitle());
        pref.setText("hello");
        Log.i(TAG, "edittext " + pref.getText() + " " + pref.getSummary());
        pref.setSummary("sum");
        pref.setText(null);
        Log.i(TAG, "edittext " + pref.getText() + " " + pref.getSummary() + " " + pref.getEditText().getClass()
                .getSimpleName());
    }

    private void list() {
        ListPreference pref = new ListPreference(this);
        pref.setKey("quality");
        pref.setEntries(new CharSequence[] {"Low", "Medium", "High"});
        pref.setEntryValues(new CharSequence[] {"low", "medium", "high"});
        Log.i(TAG, "list " + pref.getValue() + " " + pref.getEntry() + " " + pref.findIndexOfValue("high")
                + " " + pref.findIndexOfValue("none"));
        pref.setSummary("Now: %s");
        Log.i(TAG, "list " + pref.getSummary());
        pref.setValue("medium");
        Log.i(TAG, "list " + pref.getValue() + " " + pref.getEntry() + " " + pref.getSummary());
        pref.setValueIndex(2);
        Log.i(TAG, "list " + pref.getValue() + " " + pref.getEntry() + " " + pref.getEntries().length + " "
                + pref.getEntryValues().length);
        pref.setValue("other");
        Log.i(TAG, "list " + pref.getValue() + " " + pref.getEntry());
    }

    private void multi() {
        MultiSelectListPreference pref = new MultiSelectListPreference(this);
        pref.setEntries(new CharSequence[] {"A", "B", "C"});
        pref.setEntryValues(new CharSequence[] {"a", "b", "c"});
        Log.i(TAG, "multi " + new TreeSet<String>(pref.getValues()) + " " + pref.findIndexOfValue("c"));
        Set<String> chosen = new TreeSet<>();
        chosen.add("a");
        chosen.add("c");
        pref.setValues(chosen);
        Log.i(TAG, "multi " + new TreeSet<String>(pref.getValues()) + " " + pref.getEntries().length);
    }

    private void order() {
        Preference first = new Preference(this);
        first.setTitle("Banana");
        Preference second = new Preference(this);
        second.setTitle("apple");
        Preference third = new Preference(this);
        third.setTitle("Cherry");
        third.setOrder(5);
        Log.i(TAG, "order " + first.getOrder() + " " + third.getOrder() + " " + Preference.DEFAULT_ORDER);
        Log.i(TAG, "order compare " + sign(first.compareTo(second)) + " " + sign(second.compareTo(first)) + " "
                + sign(first.compareTo(third)) + " " + sign(third.compareTo(first)) + " "
                + sign(first.compareTo(first)));
    }

    private static int sign(int value) {
        return Integer.signum(value);
    }

    private interface Value {
        Object get() throws Exception;
    }

    /** Logs a call's result, or the class of what it throws, so that one failing call hides no other. */
    private static String show(Value value) {
        try {
            return String.valueOf(value.get());
        } catch (Throwable t) {
            return "!" + t.getClass().getSimpleName();
        }
    }

    private void category() {
        PreferenceCategory category = new PreferenceCategory(this);
        category.setKey("cat");
        category.setTitle("Cat");
        Preference one = new Preference(this);
        one.setKey("one");
        Preference two = new Preference(this);
        two.setKey("two");
        two.setOrder(1);
        Log.i(TAG, "category add " + show(() -> category.addPreference(one)) + " "
                + show(() -> category.addPreference(two)) + " count=" + show(category::getPreferenceCount));
        Log.i(TAG, "category get " + show(() -> category.getPreference(0).getKey()) + " "
                + show(() -> category.getPreference(1).getKey()) + " ordered="
                + show(category::isOrderingAsAdded) + " find=" + show(() -> category.findPreference("two") != null)
                + " none=" + show(() -> category.findPreference("zz") != null));
        Log.i(TAG, "category sorted " + show(() -> {
            category.setOrderingAsAdded(false);
            return category.isOrderingAsAdded();
        }) + " " + show(() -> category.getPreference(0).getKey()));
        Log.i(TAG, "category remove " + show(() -> category.removePreference(one)) + " "
                + show(() -> category.removePreference(one)) + " count=" + show(category::getPreferenceCount));
        Log.i(TAG, "category " + show(() -> {
            category.removeAll();
            return category.getPreferenceCount();
        }) + " enabled=" + category.isEnabled() + " selectable=" + category.isSelectable());
    }

    private void plain() {
        Preference pref = new Preference(this);
        pref.setKey("p");
        pref.setTitle("Title");
        pref.setSummary("Summary");
        pref.setDependency("other");
        pref.setEnabled(false);
        pref.setSelectable(false);
        pref.setPersistent(false);
        pref.setShouldDisableView(false);
        Log.i(TAG, "plain " + pref.getKey() + " " + pref.getTitle() + " " + pref.getSummary() + " " + pref.getDependency()
                + " " + pref.isEnabled() + " " + pref.isSelectable() + " " + pref.isPersistent() + " "
                + pref.getShouldDisableView());
        Log.i(TAG, "plain layouts " + show(() -> {
            pref.setLayoutResource(7);
            return pref.getLayoutResource();
        }) + " " + show(() -> {
            pref.setWidgetLayoutResource(8);
            return pref.getWidgetLayoutResource();
        }));
        Log.i(TAG, "plain shared=" + show(pref::getSharedPreferences) + " editor=" + show(pref::getEditor)
                + " manager=" + show(pref::getPreferenceManager) + " fragment=" + show(pref::getFragment)
                + " intent=" + show(pref::getIntent) + " titleRes=" + show(pref::getTitleRes)
                + " icon=" + show(pref::getIcon));
        pref.setOnPreferenceChangeListener((p, value) -> false);
        Log.i(TAG, "plain " + (pref.getOnPreferenceChangeListener() != null) + " "
                + (pref.getOnPreferenceClickListener() != null));
    }
}
