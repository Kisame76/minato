package android.media;

import android.content.ComponentName;
import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (the small batch of 2026-10-12): our class under the media module's public name, which only has to
 * load, since MediaSessionManager names it and apps reflect over the media session classes; every body logs once and
 * throws UnsupportedOperationException. A token of a media2 session or session service; the media module's sources,
 * where AOSP's class lives, are not fetched, and Minato has no media session service that would hand one out.
 */
public final class Session2Token implements Parcelable {
    public static final int TYPE_SESSION = 0;
    public static final int TYPE_SESSION_SERVICE = 1;

    public static final Parcelable.Creator<Session2Token> CREATOR = new Parcelable.Creator<Session2Token>() {
        @Override
        public Session2Token createFromParcel(Parcel in) {
            throw Unwritten.placeholder("android.media.Session2Token.CREATOR.createFromParcel(Parcel)");
        }

        @Override
        public Session2Token[] newArray(int size) {
            return new Session2Token[size];
        }
    };

    public Session2Token(Context context, ComponentName serviceComponent) {
        throw Unwritten.placeholder("android.media.Session2Token(Context, ComponentName)");
    }

    public String getPackageName() {
        throw Unwritten.placeholder("android.media.Session2Token.getPackageName()");
    }

    public String getServiceName() {
        throw Unwritten.placeholder("android.media.Session2Token.getServiceName()");
    }

    public int getType() {
        throw Unwritten.placeholder("android.media.Session2Token.getType()");
    }

    public int getUid() {
        throw Unwritten.placeholder("android.media.Session2Token.getUid()");
    }

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        throw Unwritten.placeholder("android.media.Session2Token.writeToParcel(Parcel, int)");
    }
}
