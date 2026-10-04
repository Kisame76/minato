/*
 * Interim (ADR-0039 step c2c1): android.app.Activity as a class, so that an app's AppComponentFactory that overrides
 * instantiateActivity, as androidx's CoreComponentFactory does, passes ART's check of its signatures across the class
 * loaders (ValidateSuperClassDescriptors), which resolves the return type. Nothing launches an activity before step
 * c2c3, which replaces this class with AOSP's.
 */
package android.app;

import android.content.ContextWrapper;

public class Activity extends ContextWrapper {
    public Activity() {
        super(null);
    }

    /** As AOSP's: nothing; the interim class needs it for ExpandableListActivity's API stub. */
    public void onCreateContextMenu(android.view.ContextMenu menu, android.view.View v,
            android.view.ContextMenu.ContextMenuInfo menuInfo) {
    }

    /** The activity's window: none before its attach, as AOSP's before attach; no activity starts before c2c3. */
    public android.view.Window getWindow() {
        return null;
    }

    /** As AOSP's for an activity without autofill. */
    public android.view.autofill.AutofillManager.AutofillClient getAutofillClient() {
        return null;
    }

    /** As AOSP's API 33: a dumpable for dumpsys, which no dump asks for on Minato. */
    public void addDumpable(android.util.Dumpable dumpable) {
    }
}
