package io.github.kisame76.minato.test.sms;

import android.app.Application;
import android.provider.Telephony;
import android.util.Log;
import android.util.Patterns;

/**
 * The test of android.provider.Telephony's constants and android.util.Patterns, without a window (retest 8's resolve
 * list, batch B1). It logs under the tag sms-test what the classes answer: the content URIs and columns an SMS SDK
 * reads, and what the patterns match. The default SMS package differs by device and is only shown, under
 * sms-test-device. tools/android-reference.sh takes the log from Android 15, and tools/sms-check.sh compares Minato's
 * with it.
 */
public class SmsApplication extends Application {
    private static final String TAG = "sms-test";

    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("sms", this::sms);
        part("patterns", SmsApplication::patterns);
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

    private void sms() {
        Log.i(TAG, "sms: uris " + Telephony.Sms.CONTENT_URI + " " + Telephony.Sms.Inbox.CONTENT_URI + " "
                + Telephony.Sms.Sent.CONTENT_URI + " " + Telephony.Sms.Draft.CONTENT_URI + " "
                + Telephony.Sms.Outbox.CONTENT_URI + " " + Telephony.Sms.Conversations.CONTENT_URI);
        Log.i(TAG, "sms: others " + Telephony.Threads.CONTENT_URI + " " + Telephony.Mms.CONTENT_URI + " "
                + Telephony.Carriers.CONTENT_URI + " " + Telephony.Sms.DEFAULT_SORT_ORDER);
        Log.i(TAG, "sms: types " + Telephony.Sms.MESSAGE_TYPE_ALL + " " + Telephony.Sms.MESSAGE_TYPE_INBOX + " "
                + Telephony.Sms.MESSAGE_TYPE_SENT + " " + Telephony.Sms.MESSAGE_TYPE_DRAFT + " "
                + Telephony.Sms.MESSAGE_TYPE_OUTBOX + " " + Telephony.Sms.MESSAGE_TYPE_FAILED + " "
                + Telephony.Sms.MESSAGE_TYPE_QUEUED);
        Log.i(TAG, "sms: columns " + Telephony.TextBasedSmsColumns.ADDRESS + " " + Telephony.TextBasedSmsColumns.BODY
                + " " + Telephony.TextBasedSmsColumns.DATE + " " + Telephony.TextBasedSmsColumns.READ + " "
                + Telephony.TextBasedSmsColumns.TYPE + " " + Telephony.TextBasedSmsColumns.THREAD_ID + " "
                + Telephony.TextBasedSmsColumns.SUBSCRIPTION_ID);
        Log.i(TAG, "sms: actions " + Telephony.Sms.Intents.SMS_RECEIVED_ACTION + " "
                + Telephony.Sms.Intents.SMS_DELIVER_ACTION + " " + Telephony.Sms.Intents.WAP_PUSH_DELIVER_ACTION
                + " " + Telephony.Sms.Intents.ACTION_CHANGE_DEFAULT + " "
                + Telephony.Sms.Intents.RESULT_SMS_GENERIC_ERROR + " " + Telephony.Sms.Intents.RESULT_SMS_HANDLED);
        // The default SMS app differs by device (the AVD's image has a messaging app, Minato none).
        Log.i(TAG + "-device", "sms: default package " + Telephony.Sms.getDefaultSmsPackage(this));
    }

    private static void patterns() {
        String[] emails = {"user@example.com", "a.b+c@sub.example.co.uk", "no-at.example.com", "a@b", "x@y.z"};
        for (String each : emails) {
            Log.i(TAG, "patterns: email " + each + " " + Patterns.EMAIL_ADDRESS.matcher(each).matches());
        }
        String[] urls = {"https://example.com/path?query=1#frag", "example.com", "http://localhost:8080/", "ftp://x.org",
                "not a url", "www.example.org/a_b", "http://192.168.0.1/index.html"};
        for (String each : urls) {
            Log.i(TAG, "patterns: web " + each + " " + Patterns.WEB_URL.matcher(each).matches());
        }
        String[] phones = {"+1 (555) 123-4567", "0049 30 123456", "12", "555.123.4567"};
        for (String each : phones) {
            Log.i(TAG, "patterns: phone " + each + " " + Patterns.PHONE.matcher(each).matches());
        }
        Log.i(TAG, "patterns: ip " + Patterns.IP_ADDRESS.matcher("10.0.0.255").matches() + " "
                + Patterns.IP_ADDRESS.matcher("256.1.1.1").matches() + " domain "
                + Patterns.DOMAIN_NAME.matcher("sub.example.com").matches() + " "
                + Patterns.DOMAIN_NAME.matcher("-bad-.com").matches());
    }
}
