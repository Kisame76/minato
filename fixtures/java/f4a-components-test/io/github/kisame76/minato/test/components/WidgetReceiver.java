package io.github.kisame76.minato.test.components;

import android.appwidget.AppWidgetManager;
import android.appwidget.AppWidgetProvider;
import android.content.Context;
import java.util.Arrays;
import java.util.concurrent.CopyOnWriteArrayList;

/** An app widget provider: the broadcast of an update reaches onUpdate. */
public class WidgetReceiver extends AppWidgetProvider {
    static final CopyOnWriteArrayList<String> HEARD = new CopyOnWriteArrayList<>();

    @Override
    public void onUpdate(Context context, AppWidgetManager manager, int[] appWidgetIds) {
        HEARD.add("update " + Arrays.toString(appWidgetIds) + " manager=" + (manager != null));
    }

    @Override
    public void onEnabled(Context context) {
        HEARD.add("enabled");
    }
}
