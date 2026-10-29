// The hand file of our AutofillManager (ADR-0039 step c2c3, ADR-0045 decision 3): tools/managergen writes the class
// from the public API, whose methods log once and return their default, and the members here are the hidden ones
// that taken AOSP files call. Autofill is off on Minato (ADR-0041 decision 2), as on a device without the autofill
// service: every Context gives a manager (gap 1, batch 3), as AOSP's registry makes one with a null IAutoFillManager,
// and it answers as AOSP's does then (hasAutofillFeature false): off, unsupported, no service, and every call a no-op
// [verified: system-services-test on apk-runner-35, whose image runs the service, so only "supported" differs].
// Compose's AndroidAutofill throws where a context gives none.
import android.annotation.NonNull;
import android.annotation.Nullable;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentSender;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.IBinder;
import android.view.KeyEvent;
import android.view.View;
import java.io.PrintWriter;
import com.android.minato.services.Unwritten;

class AutofillManager {
    /** @hide AutofillOptions' verbose flag (the Unity test APK retest 10: Context's methods name AutofillOptions). */
    public static final int FLAG_ADD_CLIENT_VERBOSE = 0x4;

    /** @hide SystemServiceRegistry's: the manager of a device without the autofill service. */
    public AutofillManager(Context context) {
    }

    public boolean isEnabled() {
        return false;
    }

    public boolean isAutofillSupported() {
        return false;
    }

    public boolean hasEnabledAutofillServices() {
        return false;
    }

    public ComponentName getAutofillServiceComponentName() {
        return null;
    }

    public boolean isFieldClassificationEnabled() {
        return false;
    }

    /** Kept nowhere: without the service no callback is ever called. */
    public void registerCallback(AutofillCallback callback) {
    }

    public void unregisterCallback(AutofillCallback callback) {
    }

    public void notifyViewEntered(View view) {
    }

    public void notifyViewEntered(View view, int virtualId, Rect absBounds) {
    }

    public void notifyViewExited(View view) {
    }

    public void notifyViewExited(View view, int virtualId) {
    }

    public void notifyValueChanged(View view) {
    }

    public void notifyValueChanged(View view, int virtualId, AutofillValue value) {
    }

    public void requestAutofill(View view) {
    }

    public void requestAutofill(View view, int virtualId, Rect absBounds) {
    }

    public void commit() {
    }

    public void cancel() {
    }

    /** @hide What AutofillManager asks of an activity; AutofillClientController answers it. */
    public interface AutofillClient {
        void autofillClientAuthenticate(int authenticationId, IntentSender intent, Intent fillInIntent,
                boolean authenticateInline);

        void autofillClientResetableStateAvailable();

        boolean autofillClientRequestShowFillUi(@NonNull View anchor, int width, int height,
                @Nullable Rect virtualBounds, IAutofillWindowPresenter presenter);

        void autofillClientDispatchUnhandledKey(@NonNull View anchor, @NonNull KeyEvent keyEvent);

        boolean autofillClientRequestHideFillUi();

        boolean autofillClientIsFillUiShowing();

        @NonNull boolean[] autofillClientGetViewVisibility(@NonNull AutofillId[] autofillIds);

        boolean autofillClientIsVisibleForAutofill();

        boolean isDisablingEnterExitEventForAutofill();

        @NonNull View[] autofillClientFindViewsByAutofillIdTraversal(@NonNull AutofillId[] autofillIds);

        @Nullable View autofillClientFindViewByAutofillIdTraversal(@NonNull AutofillId autofillId);

        @Nullable View autofillClientFindViewByAccessibilityIdTraversal(int viewId, int windowId);

        void autofillClientRunOnUiThread(Runnable action);

        ComponentName autofillClientGetComponentName();

        @Nullable IBinder autofillClientGetActivityToken();

        boolean autofillClientIsCompatibilityModeEnabled();

        @Nullable AutofillId autofillClientGetNextAutofillId();
    }

    /** @hide */
    public boolean isTriggerFillRequestOnFilteredImportantViewsEnabled() {
        Unwritten.log("android.view.autofill.AutofillManager.isTriggerFillRequestOnFilteredImportantViewsEnabled()",
                "false");
        return false;
    }

    /** @hide */
    public boolean isTriggerFillRequestOnUnimportantViewEnabled() {
        Unwritten.log("android.view.autofill.AutofillManager.isTriggerFillRequestOnUnimportantViewEnabled()", "false");
        return false;
    }

    /** @hide */
    public boolean shouldIncludeAllChildrenViewsWithAutofillTypeNotNoneInAssistStructure() {
        Unwritten.log("android.view.autofill.AutofillManager"
                + ".shouldIncludeAllChildrenViewsWithAutofillTypeNotNoneInAssistStructure()",
                "false");
        return false;
    }

    /** @hide */
    public boolean shouldIncludeAllChildrenViewInAssistStructure() {
        Unwritten.log("android.view.autofill.AutofillManager.shouldIncludeAllChildrenViewInAssistStructure()", "false");
        return false;
    }

    /** @hide */
    public boolean shouldAlwaysIncludeWebviewInAssistStructure() {
        Unwritten.log("android.view.autofill.AutofillManager.shouldAlwaysIncludeWebviewInAssistStructure()", "false");
        return false;
    }

    /** @hide */
    public boolean shouldIncludeInvisibleViewInAssistStructure() {
        Unwritten.log("android.view.autofill.AutofillManager.shouldIncludeInvisibleViewInAssistStructure()", "false");
        return false;
    }

    /** @hide */
    public boolean isActivityDeniedForAutofill() {
        Unwritten.log("android.view.autofill.AutofillManager.isActivityDeniedForAutofill()", "false");
        return false;
    }

    /** @hide */
    public boolean isAutofillable(View view) {
        Unwritten.log("android.view.autofill.AutofillManager.isAutofillable(View)", "false");
        return false;
    }

    /** @hide */
    public void notifyViewEnteredForFillDialog(View v) {
        Unwritten.log("android.view.autofill.AutofillManager.notifyViewEnteredForFillDialog(View)", "nothing");
    }

    /** @hide */
    public void notifyViewEnteredForAugmentedAutofill(View view) {
        Unwritten.log("android.view.autofill.AutofillManager.notifyViewEnteredForAugmentedAutofill(View)", "nothing");
    }

    /** @hide */
    public static final String EXTRA_RESTORE_SESSION_TOKEN = "android.view.autofill.extra.RESTORE_SESSION_TOKEN";

    /** @hide */
    public static final String EXTRA_RESTORE_CROSS_ACTIVITY = "android.view.autofill.extra.RESTORE_CROSS_ACTIVITY";

    /** @hide */
    public static final int PENDING_UI_OPERATION_RESTORE = 2;

    /** @hide */
    public void onCreate(Bundle savedInstanceState) {
        Unwritten.log("android.view.autofill.AutofillManager.onCreate(Bundle)", "nothing");
    }

    /** @hide */
    public void onVisibleForAutofill() {
        Unwritten.log("android.view.autofill.AutofillManager.onVisibleForAutofill()", "nothing");
    }

    /** @hide */
    public void onInvisibleForAutofill(boolean isExpiredResponse) {
        Unwritten.log("android.view.autofill.AutofillManager.onInvisibleForAutofill(boolean)", "nothing");
    }

    /** @hide */
    public void onActivityFinishing() {
        Unwritten.log("android.view.autofill.AutofillManager.onActivityFinishing()", "nothing");
    }

    /** @hide */
    public void onSaveInstanceState(Bundle outState) {
        Unwritten.log("android.view.autofill.AutofillManager.onSaveInstanceState(Bundle)", "nothing");
    }

    /** @hide */
    public void onAuthenticationResult(int authenticationId, Intent data, View focusView) {
        Unwritten.log("android.view.autofill.AutofillManager.onAuthenticationResult(int, Intent, View)", "nothing");
    }

    /** @hide */
    public void enableCompatibilityMode() {
        Unwritten.log("android.view.autofill.AutofillManager.enableCompatibilityMode()", "nothing");
    }

    /** @hide */
    public void dump(String outerPrefix, PrintWriter pw) {
        Unwritten.log("android.view.autofill.AutofillManager.dump(String, PrintWriter)", "nothing");
    }

    /** @hide */
    public void onPendingSaveUi(int operation, IBinder token) {
        Unwritten.log("android.view.autofill.AutofillManager.onPendingSaveUi(int, IBinder)", "nothing");
    }
}
