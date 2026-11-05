package io.github.kisame76.minato.test.accessibility;

import android.app.Application;
import android.content.Context;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.util.Log;
import android.view.View;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.concurrent.Callable;

/**
 * The accessibility data classes that apps and libraries fill and read without an accessibility service: a node
 * info with its flags, text, bounds, actions, collection and range infos, the node info of a view, the parcel round
 * trip, the window info and the service info. Each is logged with its result, or with the class of the exception
 * it throws. The window info and the service info are reached by name, so that the same sources build against
 * Android's SDK and against Minato's framework before it has the classes.
 */
public class AccessibilityApplication extends Application {
    private static final String TAG = "accessibility-answers-test";

    private static void log(String name, Callable<Object> call) {
        String result;
        try {
            result = String.valueOf(call.call());
        } catch (InvocationTargetException e) {
            result = "throws " + e.getCause().getClass().getName();
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(TAG, name + ": " + result);
    }

    private static boolean accepts(Method candidate, Object[] args) {
        final Class<?>[] types = candidate.getParameterTypes();
        if (types.length != args.length) return false;
        for (int i = 0; i < types.length; i++) {
            if (args[i] == null) {
                if (types[i].isPrimitive()) return false;
            } else if (types[i].isPrimitive()) {
                if (types[i] == int.class && !(args[i] instanceof Integer)) return false;
            } else if (!types[i].isInstance(args[i])) {
                return false;
            }
        }
        return true;
    }

    private static Object call(Object target, String method, Object... args) throws Exception {
        for (Method candidate : target.getClass().getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate, args)) {
                return candidate.invoke(target, args);
            }
        }
        throw new NoSuchMethodException(target.getClass().getName() + "." + method);
    }

    private static Object callStatic(String type, String method, Object... args) throws Exception {
        for (Method candidate : Class.forName(type).getMethods()) {
            if (candidate.getName().equals(method) && accepts(candidate, args)) {
                return candidate.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(type + "." + method);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private static AccessibilityNodeInfo filled() {
        final AccessibilityNodeInfo info = new AccessibilityNodeInfo();
        info.setPackageName("p.q");
        info.setClassName("android.widget.Button");
        info.setText("Play");
        info.setContentDescription("Play the game");
        info.setViewIdResourceName("p.q:id/play");
        info.setHintText("hint");
        info.setBoundsInScreen(new Rect(1, 2, 30, 40));
        info.setBoundsInParent(new Rect(0, 0, 29, 38));
        info.setClickable(true);
        info.setFocusable(true);
        info.setEnabled(true);
        info.setCheckable(true);
        info.setChecked(true);
        info.setScrollable(true);
        info.setLongClickable(true);
        info.setPassword(true);
        info.setSelected(true);
        info.setVisibleToUser(true);
        info.addAction(AccessibilityNodeInfo.ACTION_CLICK);
        info.addAction(AccessibilityNodeInfo.ACTION_FOCUS);
        info.addAction(new AccessibilityNodeInfo.AccessibilityAction(0x01000000, "Custom"));
        info.setMovementGranularities(AccessibilityNodeInfo.MOVEMENT_GRANULARITY_WORD);
        info.setMaxTextLength(20);
        info.setTextSelection(1, 3);
        info.setInputType(1);
        info.setLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        info.setCollectionInfo(AccessibilityNodeInfo.CollectionInfo.obtain(3, 2, true));
        info.setCollectionItemInfo(AccessibilityNodeInfo.CollectionItemInfo.obtain(1, 1, 0, 1, false));
        info.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_INT,
                0f, 10f, 4f));
        info.setError("bad");
        info.setPaneTitle("pane");
        info.setHeading(true);
        info.setImportantForAccessibility(true);
        return info;
    }

    private void run() {
        final AccessibilityNodeInfo info = filled();
        log("text", () -> info.getText());
        log("contentDescription", () -> info.getContentDescription());
        log("className", () -> info.getClassName());
        log("packageName", () -> info.getPackageName());
        log("viewId", () -> info.getViewIdResourceName());
        log("hint", () -> info.getHintText());
        log("boundsInScreen", () -> {
            final Rect r = new Rect();
            info.getBoundsInScreen(r);
            return r;
        });
        log("boundsInParent", () -> {
            final Rect r = new Rect();
            info.getBoundsInParent(r);
            return r;
        });
        log("flags", () -> info.isClickable() + " " + info.isFocusable() + " " + info.isEnabled() + " "
                + info.isCheckable() + " " + info.isChecked() + " " + info.isScrollable() + " "
                + info.isLongClickable() + " " + info.isPassword() + " " + info.isSelected() + " "
                + info.isVisibleToUser() + " " + info.isFocused() + " " + info.isEditable());
        log("actions", () -> info.getActions());
        log("actionList", () -> info.getActionList().size());
        log("action ids", () -> {
            final StringBuilder ids = new StringBuilder();
            for (AccessibilityNodeInfo.AccessibilityAction a : info.getActionList()) ids.append(a.getId()).append(' ');
            return ids.toString().trim();
        });
        log("custom action label", () -> info.getActionList().get(2).getLabel());
        log("ACTION_CLICK id", () -> AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK.getId());
        log("granularities", () -> info.getMovementGranularities());
        log("maxTextLength", () -> info.getMaxTextLength());
        log("textSelection", () -> info.getTextSelectionStart() + " " + info.getTextSelectionEnd());
        log("inputType", () -> info.getInputType());
        log("liveRegion", () -> info.getLiveRegion());
        log("collection", () -> info.getCollectionInfo().getRowCount() + " "
                + info.getCollectionInfo().getColumnCount() + " " + info.getCollectionInfo().isHierarchical());
        log("collection item", () -> info.getCollectionItemInfo().getRowIndex() + " "
                + info.getCollectionItemInfo().getColumnIndex() + " " + info.getCollectionItemInfo().getColumnSpan());
        log("range", () -> info.getRangeInfo().getType() + " " + info.getRangeInfo().getMin() + " "
                + info.getRangeInfo().getMax() + " " + info.getRangeInfo().getCurrent());
        log("error", () -> info.getError());
        log("paneTitle", () -> info.getPaneTitle());
        log("heading", () -> info.isHeading());
        log("childCount", () -> info.getChildCount());
        log("parent", () -> info.getParent());
        log("window", () -> call(info, "getWindow"));
        log("windowId", () -> info.getWindowId());
        log("refresh", () -> info.refresh());
        log("performAction(click)", () -> info.performAction(AccessibilityNodeInfo.ACTION_CLICK));
        log("describeContents", () -> info.describeContents());

        log("copy equals", () -> AccessibilityNodeInfo.obtain(info).equals(info));
        log("copy text", () -> AccessibilityNodeInfo.obtain(info).getText());
        log("parcel round trip", () -> {
            final Parcel parcel = Parcel.obtain();
            info.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            final AccessibilityNodeInfo back = AccessibilityNodeInfo.CREATOR.createFromParcel(parcel);
            parcel.recycle();
            return back.getText() + "|" + back.getClassName() + "|" + back.getActionList().size() + "|"
                    + back.getCollectionInfo().getRowCount() + "|" + back.getRangeInfo().getCurrent() + "|"
                    + back.isChecked() + "|" + back.getError();
        });
        log("toString has class", () -> info.toString().contains("android.widget.Button"));

        final View view = new View(this);
        view.setContentDescription("a view");
        view.setClickable(true);
        log("view node class", () -> view.createAccessibilityNodeInfo().getClassName());
        log("view node description", () -> view.createAccessibilityNodeInfo().getContentDescription());
        log("view node package", () -> view.createAccessibilityNodeInfo().getPackageName());
        log("view node clickable", () -> view.createAccessibilityNodeInfo().isClickable());
        log("view node enabled", () -> view.createAccessibilityNodeInfo().isEnabled());
        log("view node action CLICK", () -> (view.createAccessibilityNodeInfo().getActions()
                & AccessibilityNodeInfo.ACTION_CLICK) != 0);

        final AccessibilityManager manager = (AccessibilityManager) getSystemService(Context.ACCESSIBILITY_SERVICE);
        log("manager isEnabled", () -> manager.isEnabled());
        log("manager isTouchExplorationEnabled", () -> manager.isTouchExplorationEnabled());
        log("manager enabled services", () -> manager.getEnabledAccessibilityServiceList(-1).size());

        final String window = "android.view.accessibility.AccessibilityWindowInfo";
        log("window obtain type", () -> call(callStatic(window, "obtain"), "getType"));
        log("window obtain id", () -> call(callStatic(window, "obtain"), "getId"));
        log("window obtain layer", () -> call(callStatic(window, "obtain"), "getLayer"));
        log("window obtain active", () -> call(callStatic(window, "obtain"), "isActive"));
        log("window obtain focused", () -> call(callStatic(window, "obtain"), "isFocused"));
        log("window obtain childCount", () -> call(callStatic(window, "obtain"), "getChildCount"));
        log("window obtain parent", () -> call(callStatic(window, "obtain"), "getParent"));
        log("window obtain root", () -> call(callStatic(window, "obtain"), "getRoot"));
        log("window obtain title", () -> call(callStatic(window, "obtain"), "getTitle"));
        log("window obtain bounds", () -> {
            final Rect r = new Rect();
            call(callStatic(window, "obtain"), "getBoundsInScreen", r);
            return r;
        });
        log("window TYPE_APPLICATION", () -> Class.forName(window).getField("TYPE_APPLICATION").get(null));

        final String service = "android.accessibilityservice.AccessibilityServiceInfo";
        log("service info defaults", () -> {
            final Object serviceInfo = Class.forName(service).getConstructor().newInstance();
            return call(serviceInfo, "getCapabilities") + " " + call(serviceInfo, "getId") + " "
                    + call(serviceInfo, "getResolveInfo") + " " + call(serviceInfo, "getSettingsActivityName");
        });
        log("service info fields", () -> {
            final Object serviceInfo = Class.forName(service).getConstructor().newInstance();
            final Class<?> type = serviceInfo.getClass();
            type.getField("eventTypes").setInt(serviceInfo, 32);
            type.getField("feedbackType").setInt(serviceInfo, 1);
            type.getField("notificationTimeout").setLong(serviceInfo, 100L);
            type.getField("flags").setInt(serviceInfo, 2);
            type.getField("packageNames").set(serviceInfo, new String[] {"a.b"});
            return type.getField("eventTypes").getInt(serviceInfo) + " "
                    + type.getField("feedbackType").getInt(serviceInfo)
                    + " " + type.getField("notificationTimeout").getLong(serviceInfo) + " "
                    + type.getField("flags").getInt(serviceInfo) + " "
                    + ((String[]) type.getField("packageNames").get(serviceInfo))[0];
        });
        log("service info feedbackTypeToString", () -> callStatic(service, "feedbackTypeToString", 1));
        log("service info flagToString", () -> callStatic(service, "flagToString", 2));
        log("service info capabilityToString", () -> callStatic(service, "capabilityToString", 1));
        log("service info parcel round trip", () -> {
            final Object serviceInfo = Class.forName(service).getConstructor().newInstance();
            final Class<?> type = serviceInfo.getClass();
            type.getField("eventTypes").setInt(serviceInfo, 32);
            type.getField("notificationTimeout").setLong(serviceInfo, 100L);
            final Parcel parcel = Parcel.obtain();
            call(serviceInfo, "writeToParcel", parcel, 0);
            parcel.setDataPosition(0);
            final Object back = ((android.os.Parcelable.Creator<?>) type.getField("CREATOR").get(null))
                    .createFromParcel(parcel);
            parcel.recycle();
            return type.getField("eventTypes").getInt(back) + " " + type.getField("notificationTimeout").getLong(back);
        });
        Log.i(TAG, "done");
    }
}
