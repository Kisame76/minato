/*
 * The providers of a phone's Calendar, call log, voicemail and blocked numbers, empty (the provider contracts, group
 * 2 of the framework breadth; provider-contracts-test): Android's system_server publishes them to an app that asks
 * for their authority, and Minato has no such data, so ActivityThread.acquireProvider gives this local provider as it
 * gives the settings and media ones. It answers as a fresh phone's do [verified: provider-contracts-test and its
 * -granted twin on apk-runner-35]:
 *   - Calendar and the call log: an app without their read or write permission is refused when it asks for the
 *     provider, as ActivityManagerService refuses it ("Permission Denial: opening provider ..."); with one of them a
 *     query gives no rows and the provider's columns, and an insert without values gives null;
 *   - voicemail: its own check in the provider, ADD_VOICEMAIL, as ContentProvider's Transport words it, and for an app
 *     that holds it the provider's own refusal of a caller that is not the voicemail source;
 *   - blocked numbers: every call and query is refused to an app that is not the system, the default dialer or the
 *     default SMS app, and canCurrentUserBlockNumbers is true, as on a phone.
 * No data of iOS's calendar or call history is read: a calendar that a user of Minato has stays out of reach (a named
 * limit). The column lists are those of a fresh AVD's cursors in their order.
 */
package android.provider;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Process;

/** @hide */
public final class LocalContractProvider extends ContentProvider {
    public static final String CALENDAR = "com.android.calendar";
    public static final String CALL_LOG = "call_log";
    public static final String VOICEMAIL = "com.android.voicemail";
    public static final String BLOCKED_NUMBERS = "com.android.blockednumber";

    private static final String READ_CALENDAR = "android.permission.READ_CALENDAR";
    private static final String WRITE_CALENDAR = "android.permission.WRITE_CALENDAR";
    private static final String READ_CALL_LOG = "android.permission.READ_CALL_LOG";
    private static final String WRITE_CALL_LOG = "android.permission.WRITE_CALL_LOG";
    private static final String ADD_VOICEMAIL = "com.android.voicemail.permission.ADD_VOICEMAIL";
    private static final String WRITE_VOICEMAIL = "com.android.voicemail.permission.WRITE_VOICEMAIL";

    private static final String CALENDAR_CLASS = "com.android.providers.calendar.CalendarProvider2";
    private static final String CALL_LOG_CLASS = "com.android.providers.contacts.CallLogProvider";
    private static final String VOICEMAIL_CLASS = "com.android.providers.contacts.VoicemailContentProvider";
    private static final String BLOCKED_CLASS = "com.android.providers.blockednumber.BlockedNumberProvider";

    private static final String[] CALENDARS = {
        "account_type", "mutators", "ownerAccount", "allowedReminders", "cal_sync3", "cal_sync2", "isPrimary",
        "maxReminders", "cal_sync1", "cal_sync10", "account_name", "cal_sync7", "cal_sync6", "canPartiallyUpdate",
        "cal_sync5", "sync_events", "cal_sync4", "canOrganizerRespond", "calendar_color", "cal_sync9",
        "calendar_location", "cal_sync8", "dirty", "visible", "calendar_timezone", "calendar_access_level",
        "allowedAvailability", "_sync_id", "deleted", "name", "canModifyTimeZone", "_id", "calendar_color_index",
        "allowedAttendeeTypes", "calendar_displayName",
    };

    private static final String[] EVENTS = {
        "originalAllDay", "account_type", "exrule", "mutators", "originalInstanceTime", "allDay", "allowedReminders",
        "rrule", "canOrganizerRespond", "lastDate", "visible", "calendar_id", "hasExtendedProperties",
        "calendar_access_level", "selfAttendeeStatus", "allowedAvailability", "eventColor_index", "isOrganizer",
        "_sync_id", "calendar_color_index", "_id", "guestsCanInviteOthers", "allowedAttendeeTypes", "dtstart",
        "guestsCanSeeGuests", "sync_data9", "sync_data8", "exdate", "sync_data7", "sync_data6", "sync_data1",
        "description", "eventTimezone", "availability", "title", "ownerAccount", "sync_data5", "sync_data4",
        "sync_data3", "sync_data2", "duration", "lastSynced", "guestsCanModify", "cal_sync3", "rdate", "cal_sync2",
        "maxReminders", "isPrimary", "cal_sync1", "cal_sync10", "account_name", "cal_sync7", "cal_sync6", "cal_sync5",
        "cal_sync4", "calendar_color", "cal_sync9", "cal_sync8", "dirty", "calendar_timezone", "accessLevel",
        "eventLocation", "hasAlarm", "uid2445", "deleted", "eventColor", "organizer", "eventStatus", "customAppUri",
        "canModifyTimeZone", "eventEndTimezone", "customAppPackage", "original_sync_id", "hasAttendeeData",
        "displayColor", "dtend", "original_id", "sync_data10", "calendar_displayName",
    };

    private static final String[] ATTENDEES = {
        "originalAllDay", "account_type", "exrule", "attendeeStatus", "originalInstanceTime", "attendeeName",
        "allDay", "rrule", "lastDate", "calendar_id", "hasExtendedProperties", "selfAttendeeStatus",
        "eventColor_index", "isOrganizer", "_sync_id", "_id", "attendeeIdNamespace", "guestsCanInviteOthers",
        "dtstart", "guestsCanSeeGuests", "exdate", "attendeeEmail", "description", "eventTimezone", "availability",
        "title", "duration", "attendeeIdentity", "guestsCanModify", "rdate", "account_name", "accessLevel",
        "eventLocation", "attendeeType", "hasAlarm", "uid2445", "deleted", "event_id", "eventColor", "organizer",
        "eventStatus", "customAppUri", "attendeeRelationship", "eventEndTimezone", "customAppPackage",
        "original_sync_id", "hasAttendeeData", "dtend", "original_id",
    };

    private static final String[] REMINDERS = {
        "_id", "event_id", "minutes", "method",
    };

    private static final String[] INSTANCES = {
        "originalAllDay", "account_type", "exrule", "originalInstanceTime", "allDay", "allowedReminders", "rrule",
        "canOrganizerRespond", "lastDate", "visible", "calendar_id", "startDay", "hasExtendedProperties",
        "calendar_access_level", "selfAttendeeStatus", "allowedAvailability", "eventColor_index", "isOrganizer",
        "_sync_id", "calendar_color_index", "_id", "guestsCanInviteOthers", "allowedAttendeeTypes", "begin",
        "dtstart", "guestsCanSeeGuests", "exdate", "description", "eventTimezone", "availability", "title",
        "ownerAccount", "duration", "guestsCanModify", "rdate", "maxReminders", "isPrimary", "account_name", "endDay",
        "end", "calendar_color", "endMinute", "calendar_timezone", "accessLevel", "eventLocation", "hasAlarm",
        "uid2445", "deleted", "event_id", "eventColor", "organizer", "eventStatus", "customAppUri",
        "canModifyTimeZone", "eventEndTimezone", "customAppPackage", "original_sync_id", "hasAttendeeData",
        "displayColor", "dtend", "original_id", "startMinute", "calendar_displayName",
    };

    private static final String[] CALLS = {
        "formatted_number", "duration", "subject", "is_call_log_phone_account_migration_pending", "subscription_id",
        "photo_id", "post_dial_digits", "call_screening_app_name", "priority", "number", "countryiso", "photo_uri",
        "geocoded_location", "missed_reason", "is_business_call", "block_reason", "subscription_component_name",
        "add_for_all_users", "numbertype", "features", "transcription", "last_modified", "_id", "new", "date", "name",
        "type", "presentation", "via_number", "numberlabel", "normalized_number", "composer_photo_uri",
        "phone_account_address", "phone_account_hidden", "lookup_uri", "voicemail_uri", "matched_number",
        "transcription_state", "data_usage", "location", "asserted_display_name", "call_screening_component_name",
        "is_read",
    };

    private final String mAuthority;

    private LocalContractProvider(String authority) {
        mAuthority = authority;
    }

    /** Whether Minato answers for this authority. */
    public static boolean serves(String authority) {
        return CALENDAR.equals(authority) || CALL_LOG.equals(authority) || VOICEMAIL.equals(authority)
                || BLOCKED_NUMBERS.equals(authority);
    }

    /**
     * The provider for `authority`, attached to `context`; throws what ActivityManagerService throws when the app
     * holds neither permission that the provider's manifest asks for.
     */
    public static LocalContractProvider acquire(Context context, String authority) {
        ProviderInfo info = new ProviderInfo();
        info.authority = authority;
        info.exported = true;
        info.applicationInfo = context.getApplicationInfo();
        String read = null;
        String write = null;
        switch (authority) {
            case CALENDAR:
                info.name = CALENDAR_CLASS;
                info.packageName = "com.android.providers.calendar";
                read = READ_CALENDAR;
                write = WRITE_CALENDAR;
                break;
            case CALL_LOG:
                info.name = CALL_LOG_CLASS;
                info.packageName = "com.android.providers.contacts";
                read = READ_CALL_LOG;
                write = WRITE_CALL_LOG;
                break;
            case VOICEMAIL:
                info.name = VOICEMAIL_CLASS;
                info.packageName = "com.android.providers.contacts";
                break;
            default:
                info.name = BLOCKED_CLASS;
                info.packageName = "com.android.providers.blockednumber";
                break;
        }
        if (read != null && !holds(context, read) && !holds(context, write)) {
            throw new SecurityException("Permission Denial: opening provider " + info.name + " from ProcessRecord{"
                    + Integer.toHexString(System.identityHashCode(context)) + " " + Process.myPid() + ":"
                    + context.getPackageName() + "/u0a" + (Process.myUid() - Process.FIRST_APPLICATION_UID)
                    + "} (pid=" + Process.myPid() + ", uid=" + Process.myUid() + ") requires " + read + " or "
                    + write);
        }
        LocalContractProvider provider = new LocalContractProvider(authority);
        provider.attachInfo(context, info);
        return provider;
    }

    private static boolean holds(Context context, String permission) {
        return context.checkPermission(permission, Process.myPid(), Process.myUid())
                == PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        switch (mAuthority) {
            case VOICEMAIL:
                voicemailCheck("reading", uri);
                break;
            case BLOCKED_NUMBERS:
                throw new SecurityException("Caller must be system, default dialer or default SMS app");
            default:
                break;
        }
        String[] columns = columnsOf(uri);
        return new MatrixCursor(columns == null ? new String[] {"_id"} : columns, 0);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        switch (mAuthority) {
            case VOICEMAIL:
                voicemailCheck("writing", uri);
                break;
            case BLOCKED_NUMBERS:
                throw new SecurityException("Caller must be system, default dialer or default SMS app");
            default:
                break;
        }
        return null;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        if (BLOCKED_NUMBERS.equals(mAuthority)) {
            throw new SecurityException("Caller must be system, default dialer or default SMS app");
        }
        return 0;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        if (BLOCKED_NUMBERS.equals(mAuthority)) {
            throw new SecurityException("Caller must be system, default dialer or default SMS app");
        }
        return 0;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        if (!BLOCKED_NUMBERS.equals(mAuthority)) {
            return super.call(method, arg, extras);
        }
        if (BlockedNumberContract.METHOD_CAN_CURRENT_USER_BLOCK_NUMBERS.equals(method)) {
            Bundle result = new Bundle();
            result.putBoolean(BlockedNumberContract.RES_CAN_BLOCK_NUMBERS, true);
            return result;
        }
        throw new SecurityException("Caller must be system, default dialer or default SMS app");
    }

    /** The voicemail provider's two refusals: no ADD_VOICEMAIL, and an app that is not the voicemail source. */
    private void voicemailCheck(String verb, Uri uri) {
        Context context = getContext();
        if (!holds(context, ADD_VOICEMAIL)) {
            throw new SecurityException("Permission Denial: " + verb + " " + VOICEMAIL_CLASS + " uri " + uri
                    + " from pid=" + Process.myPid() + ", uid=" + Process.myUid() + " requires " + ADD_VOICEMAIL
                    + ", or grantUriPermission()");
        }
        throw new SecurityException("Provider " + context.getPackageName() + " does not have " + WRITE_VOICEMAIL
                + " permission.\nPlease set query parameter 'source_package' in the URI.\nURI: " + uri);
    }

    private String[] columnsOf(Uri uri) {
        String first = uri.getPathSegments().isEmpty() ? "" : uri.getPathSegments().get(0);
        switch (first) {
            case "calendars":
                return CALENDARS;
            case "events":
                return EVENTS;
            case "attendees":
                return ATTENDEES;
            case "reminders":
                return REMINDERS;
            case "instances":
                return INSTANCES;
            case "calls":
                return CALLS;
            default:
                return null;
        }
    }
}
