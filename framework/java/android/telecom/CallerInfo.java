/*
 * The type that CallLog's hidden addCall methods and its AddCallParams take for the contact of a call, which telecom's
 * services fill. Telecom's own CallerInfo looks a contact up through the contacts
 * provider and pulls in Phone, InCallAdapter and the call's parcels, none of which an app process runs; an app cannot
 * name this hidden type, so only its being there matters: CallLog loads, and a call that hands one in finds a plain
 * object with the fields an empty lookup leaves.
 */
package android.telecom;

import android.net.Uri;

/** @hide */
public class CallerInfo {
    public String name;
    public String phoneNumber;
    public String normalizedNumber;
    public String geoDescription;
    public Uri contactRefUri;
    public long contactIdOrZero;
    public int numberType;
    public String numberLabel;
    public int numberPresentation;
    public int namePresentation;
    public boolean contactExists;
    public String lookupKey;
    public String contactDisplayPhotoUri;
    public boolean isCachedPhotoCurrent;
    public boolean needUpdate;
    public boolean isEmergencyNumber;
    public boolean isVoiceMailNumber;
    public boolean phoneNumberPresentationIsAllowed;

    public CallerInfo() {
    }

    public String getName() {
        return name;
    }

    public long getContactId() {
        return contactIdOrZero;
    }

    public String getPhoneNumber() {
        return phoneNumber;
    }
}
