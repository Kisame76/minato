package com.android.internal.minato.picker;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ResolveInfo;

/**
 * The five intents that Android answers with a system picker (ADR-0057 decision 1) and the framework component that
 * answers them in Minato: what they resolve to, how an activity for one is made, and the request the host gets for
 * it. Plain Java apart from the Android types it names.
 */
public final class PickerIntents {
    public static final int NONE = 0;
    public static final int OPEN_DOCUMENT = 1;
    public static final int GET_CONTENT = 2;
    public static final int CREATE_DOCUMENT = 3;
    public static final int OPEN_DOCUMENT_TREE = 4;
    public static final int PICK_IMAGES = 5;

    /** MediaStore.ACTION_PICK_IMAGES, which MediaStore's compile-only stub does not name. */
    static final String ACTION_PICK_IMAGES = "android.provider.action.PICK_IMAGES";
    static final String EXTRA_PICK_IMAGES_MAX = "android.provider.extra.PICK_IMAGES_MAX";

    /** The framework class that stands in for the picker; the intent resolves to Android's component names. */
    static final String PICKER_CLASS = "com.android.internal.minato.picker.SystemPickerActivity";

    private PickerIntents() {
    }

    /** Which picker intent this is, or NONE: the action decides, for an intent that names no package. */
    public static int kindOf(Intent intent) {
        final String action = intent.getAction();
        // An intent aimed at a package is not the system picker's, whatever its action.
        if (action == null || intent.getPackage() != null) {
            return NONE;
        }
        switch (action) {
            case Intent.ACTION_OPEN_DOCUMENT:
                return OPEN_DOCUMENT;
            case Intent.ACTION_GET_CONTENT:
                return GET_CONTENT;
            case Intent.ACTION_CREATE_DOCUMENT:
                return CREATE_DOCUMENT;
            case Intent.ACTION_OPEN_DOCUMENT_TREE:
                return OPEN_DOCUMENT_TREE;
            case ACTION_PICK_IMAGES:
                return PICK_IMAGES;
            default:
                return NONE;
        }
    }

    /** The activity of the framework class for `kind`, in the app's package as an activity of the app is. */
    public static ActivityInfo activityInfo(ApplicationInfo app, int kind) {
        final ActivityInfo info = new ActivityInfo();
        info.packageName = app.packageName;
        info.name = PICKER_CLASS;
        info.applicationInfo = app;
        info.processName = app.processName;
        info.exported = false;
        info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        // A change of the configuration does not recreate the picker, which would ask the host a second time.
        info.configChanges = -1;
        info.theme = kind == PICK_IMAGES ? com.android.internal.R.style.Theme_Translucent_NoTitleBar : 0;
        return info;
    }

    /** What a picker intent resolves to when the app has no component for it: Android's, by name. */
    public static ResolveInfo resolveInfo(int kind) {
        final ResolveInfo resolved = new ResolveInfo();
        final ActivityInfo info = new ActivityInfo();
        info.packageName = packageOf(kind);
        info.name = classOf(kind);
        info.exported = true;
        info.applicationInfo = new ApplicationInfo();
        info.applicationInfo.packageName = info.packageName;
        resolved.activityInfo = info;
        resolved.match = 0x108000;
        resolved.priority = 0;
        resolved.preferredOrder = 0;
        resolved.isDefault = true;
        return resolved;
    }

    static String packageOf(int kind) {
        return kind == PICK_IMAGES ? "com.android.providers.media.module" : "com.android.documentsui";
    }

    static String classOf(int kind) {
        return kind == PICK_IMAGES ? "com.android.providers.media.photopicker.PhotoPickerActivity"
                : "com.android.documentsui.picker.PickActivity";
    }

    /** Android's component for `kind`, as an app sees it. */
    public static ComponentName componentOf(int kind) {
        return new ComponentName(packageOf(kind), classOf(kind));
    }
}
