/*
 * The PendingIntents of Minato's one app process (ADR-0045 step d3): what system_server's PendingIntentController and
 * PendingIntentRecord answer an app with, kept in the process, for LocalActivityManagerService. As at
 * android-15.0.0_r1 [verified: pendingintent-test on apk-runner-35]:
 *   - a request names its kind, package, feature, activity token and who, request code, Intent, resolved type, flags
 *     and user; two requests are one PendingIntent when all of them are equal, the Intent by filterEquals (its extras
 *     do not count) and the flags without NO_CREATE, CANCEL_CURRENT and UPDATE_CURRENT;
 *   - NO_CREATE gives the one there or null, CANCEL_CURRENT cancels the one there and makes a new one, UPDATE_CURRENT
 *     gives the one there with the new Intent's extras; a broadcast takes one Intent;
 *   - a send of a cancelled one answers START_CANCELED; a send fills a mutable one's Intent in from the sender's
 *     (Intent.fillIn with the flags), an immutable one's not; a one-shot one is cancelled by its first send; a
 *     broadcast goes out through the activity manager, ordered when the sender asks for the result, an activity
 *     through the activity task manager; a sender waiting for the result of what is no broadcast gets it at once;
 *   - cancelling tells the listeners registered for it, once.
 * The PendingIntents live as long as the process: Android forgets one nothing refers to any more, which only a later
 * NO_CREATE request can tell. Not written: a service's start (the activity manager has no services yet), the intent
 * of a PendingIntent and its tag (permissions only the system holds), and whether it targets a package.
 */
package android.app;

import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.UserHandle;

import com.android.internal.os.IResultReceiver;
import com.android.minato.services.Unwritten;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Objects;

final class LocalPendingIntents {
    /** The flags that only steer the request and are no part of a PendingIntent's identity. */
    private static final int REQUEST_FLAGS = PendingIntent.FLAG_NO_CREATE | PendingIntent.FLAG_CANCEL_CURRENT
            | PendingIntent.FLAG_UPDATE_CURRENT;

    /** PendingIntentRecord.Key: what makes two requests one PendingIntent. */
    private static final class Key {
        final int type;
        final String packageName;
        final String featureId;
        final IBinder activity;
        final String who;
        final int requestCode;
        final Intent requestIntent;
        final String requestResolvedType;
        final int flags;
        final int userId;
        Intent[] allIntents;
        String[] allResolvedTypes;

        Key(int type, String packageName, String featureId, IBinder activity, String who, int requestCode,
                Intent[] intents, String[] resolvedTypes, int flags, int userId) {
            this.type = type;
            this.packageName = packageName;
            this.featureId = featureId;
            this.activity = activity;
            this.who = who;
            this.requestCode = requestCode;
            this.requestIntent = intents != null ? intents[intents.length - 1] : null;
            this.requestResolvedType = resolvedTypes != null ? resolvedTypes[resolvedTypes.length - 1] : null;
            this.flags = flags;
            this.userId = userId;
            this.allIntents = intents;
            this.allResolvedTypes = resolvedTypes;
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof Key)) {
                return false;
            }
            Key key = (Key) other;
            return type == key.type && requestCode == key.requestCode && flags == key.flags && userId == key.userId
                    && activity == key.activity && Objects.equals(packageName, key.packageName)
                    && Objects.equals(featureId, key.featureId) && Objects.equals(who, key.who)
                    && Objects.equals(requestResolvedType, key.requestResolvedType)
                    && (requestIntent == null ? key.requestIntent == null
                            : key.requestIntent != null && requestIntent.filterEquals(key.requestIntent));
        }

        @Override
        public int hashCode() {
            return Objects.hash(type, packageName, featureId, activity, who, requestCode,
                    requestIntent != null ? requestIntent.filterHashCode() : 0, requestResolvedType, flags, userId);
        }
    }

    /** One PendingIntent: the Binder its PendingIntents share, so that they are equal. */
    final class Record extends IIntentSender.Stub {
        final Key key;
        boolean canceled;
        final List<IResultReceiver> cancelListeners = new ArrayList<>();

        Record(Key key) {
            this.key = key;
        }

        @Override
        public void send(int code, Intent intent, String resolvedType, IBinder whitelistToken,
                IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
            sendInner(this, code, intent, resolvedType, finishedReceiver, requiredPermission, options);
        }
    }

    private final LocalActivityManagerService mService;
    private final HashMap<Key, Record> mRecords = new HashMap<>();

    LocalPendingIntents(LocalActivityManagerService service) {
        mService = service;
    }

    /** ActivityManagerService's getIntentSenderWithFeature, through PendingIntentController.getIntentSender. */
    IIntentSender get(int type, String packageName, String featureId, IBinder token, String resultWho,
            int requestCode, Intent[] intents, String[] resolvedTypes, int flags, int userId) {
        if (intents != null) {
            if (intents.length < 1) {
                throw new IllegalArgumentException("Intents array length must be >= 1");
            }
            for (int i = 0; i < intents.length; i++) {
                if (intents[i] == null) {
                    throw new IllegalArgumentException("Null intent at index " + i);
                }
                if (intents[i].hasFileDescriptors()) {
                    throw new IllegalArgumentException("File descriptors passed in Intent");
                }
                if (type == ActivityManager.INTENT_SENDER_BROADCAST
                        && (intents[i].getFlags() & Intent.FLAG_RECEIVER_BOOT_UPGRADE) != 0) {
                    throw new IllegalArgumentException("Can't use FLAG_RECEIVER_BOOT_UPGRADE here");
                }
                intents[i] = new Intent(intents[i]);
            }
            if (resolvedTypes != null && resolvedTypes.length != intents.length) {
                throw new IllegalArgumentException("Intent array length does not match resolvedTypes length");
            }
            if (type == ActivityManager.INTENT_SENDER_BROADCAST && intents.length != 1) {
                throw new IllegalArgumentException("Intents array length must be 1 for broadcasts");
            }
        }
        boolean noCreate = (flags & PendingIntent.FLAG_NO_CREATE) != 0;
        boolean cancelCurrent = (flags & PendingIntent.FLAG_CANCEL_CURRENT) != 0;
        boolean updateCurrent = (flags & PendingIntent.FLAG_UPDATE_CURRENT) != 0;
        Key key = new Key(type, packageName, featureId, token, resultWho, requestCode, intents, resolvedTypes,
                flags & ~REQUEST_FLAGS, userId);
        Record cancelled = null;
        Record created = null;
        synchronized (mRecords) {
            Record record = mRecords.get(key);
            if (record != null) {
                if (!cancelCurrent) {
                    if (updateCurrent) {
                        if (record.key.requestIntent != null) {
                            record.key.requestIntent.replaceExtras(intents != null
                                    ? intents[intents.length - 1] : null);
                        }
                        if (intents != null) {
                            intents[intents.length - 1] = record.key.requestIntent;
                            record.key.allIntents = intents;
                            record.key.allResolvedTypes = resolvedTypes;
                        } else {
                            record.key.allIntents = null;
                            record.key.allResolvedTypes = null;
                        }
                    }
                    return record;
                }
                record.canceled = true;
                mRecords.remove(key);
                cancelled = record;
            }
            if (!noCreate) {
                created = new Record(key);
                mRecords.put(key, created);
            }
        }
        // Outside the lock, as cancel's: a listener may register or cancel from another thread.
        if (cancelled != null) {
            notifyCancel(cancelled);
        }
        return noCreate ? cancelled : created;
    }

    /** cancelIntentSender: the record is cancelled and forgotten, and its listeners hear of it. */
    void cancel(IIntentSender sender) {
        if (!(sender instanceof Record)) {
            return;
        }
        Record record = (Record) sender;
        synchronized (mRecords) {
            if (record.canceled) {
                return;
            }
            record.canceled = true;
            if (mRecords.get(record.key) == record) {
                mRecords.remove(record.key);
            }
        }
        notifyCancel(record);
    }

    private void notifyCancel(Record record) {
        List<IResultReceiver> listeners;
        synchronized (mRecords) {
            listeners = new ArrayList<>(record.cancelListeners);
            record.cancelListeners.clear();
        }
        for (IResultReceiver listener : listeners) {
            try {
                listener.send(Activity.RESULT_CANCELED, null);
            } catch (RemoteException e) {
                // A local receiver: none is thrown.
            }
        }
    }

    boolean registerCancelListener(IIntentSender sender, IResultReceiver receiver) {
        if (!(sender instanceof Record)) {
            return false;
        }
        Record record = (Record) sender;
        synchronized (mRecords) {
            if (record.canceled) {
                return false;
            }
            record.cancelListeners.add(receiver);
            return true;
        }
    }

    void unregisterCancelListener(IIntentSender sender, IResultReceiver receiver) {
        if (sender instanceof Record) {
            synchronized (mRecords) {
                ((Record) sender).cancelListeners.remove(receiver);
            }
        }
    }

    ActivityManager.PendingIntentInfo info(IIntentSender sender) {
        if (!(sender instanceof Record)) {
            return null;
        }
        Key key = ((Record) sender).key;
        return new ActivityManager.PendingIntentInfo(key.packageName, UserHandle.getUid(key.userId,
                UserHandle.getAppId(Process.myUid())), (key.flags & PendingIntent.FLAG_IMMUTABLE) != 0, key.type);
    }

    /** sendIntentSender: a PendingIntent of this process goes through sendInner, another sender gets the call. */
    int send(IIntentSender target, int code, Intent intent, String resolvedType, IIntentReceiver finishedReceiver,
            String requiredPermission, Bundle options) {
        if (target instanceof Record) {
            return sendInner((Record) target, code, intent, resolvedType, finishedReceiver, requiredPermission,
                    options);
        }
        try {
            target.send(code, intent, resolvedType, null, finishedReceiver, requiredPermission, options);
        } catch (RemoteException e) {
            // A local sender: none is thrown.
        }
        return ActivityManager.START_SUCCESS;
    }

    /** PendingIntentRecord.sendInner for one process. */
    private int sendInner(Record record, int code, Intent intent, String resolvedType,
            IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
        Key key = record.key;
        Intent finalIntent;
        synchronized (mRecords) {
            if (record.canceled) {
                return ActivityManager.START_CANCELED;
            }
            if ((key.flags & PendingIntent.FLAG_ONE_SHOT) != 0) {
                record.canceled = true;
                if (mRecords.get(key) == record) {
                    mRecords.remove(key);
                }
            }
            finalIntent = key.requestIntent != null ? new Intent(key.requestIntent) : new Intent();
        }
        if ((key.flags & PendingIntent.FLAG_ONE_SHOT) != 0) {
            notifyCancel(record);
        }
        if ((key.flags & PendingIntent.FLAG_IMMUTABLE) == 0 && intent != null) {
            int changes = finalIntent.fillIn(intent, key.flags);
            if ((changes & Intent.FILL_IN_DATA) == 0) {
                resolvedType = key.requestResolvedType;
            }
        } else {
            resolvedType = key.requestResolvedType;
        }
        int result;
        boolean sendFinish = finishedReceiver != null;
        switch (key.type) {
            case ActivityManager.INTENT_SENDER_BROADCAST:
                result = mService.broadcastIntentWithFeature(null, key.featureId, finalIntent, resolvedType,
                        finishedReceiver, code, null, null,
                        requiredPermission != null ? new String[] {requiredPermission} : null, null, null, -1,
                        options, finishedReceiver != null, false, key.userId);
                if (result == ActivityManager.BROADCAST_SUCCESS) {
                    sendFinish = false;
                }
                break;
            case ActivityManager.INTENT_SENDER_ACTIVITY:
                try {
                    result = ActivityTaskManager.getService().startActivity(null, key.packageName, key.featureId,
                            finalIntent, resolvedType, null, null, 0, 0, null, options);
                } catch (RemoteException e) {
                    result = ActivityManager.START_CANCELED;
                }
                break;
            case ActivityManager.INTENT_SENDER_SERVICE:
            case ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE:
                // The activity manager has no services yet (ADR-0045 decision 5).
                Unwritten.log("android.app.PendingIntent's send of a service (ADR-0045 step d3)", "START_SUCCESS");
                result = ActivityManager.START_SUCCESS;
                break;
            default:
                result = ActivityManager.START_SUCCESS;
                break;
        }
        if (sendFinish && result != ActivityManager.START_CANCELED) {
            try {
                finishedReceiver.performReceive(new Intent(finalIntent), 0, null, null, false, false, key.userId);
            } catch (RemoteException e) {
                // A local receiver: none is thrown.
            }
        }
        return result;
    }
}
