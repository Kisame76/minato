package io.github.kisame76.minato.test.serviceexceptions;

import android.app.Application;
import android.app.BackgroundServiceStartNotAllowedException;
import android.app.ForegroundServiceStartNotAllowedException;
import android.app.ForegroundServiceTypeException;
import android.app.InvalidForegroundServiceTypeException;
import android.app.MissingForegroundServiceTypeException;
import android.app.ServiceStartNotAllowedException;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.Log;

/**
 * The test of the exceptions a service start can throw, without a window (framework breadth, step (b)). It logs under
 * the tag service-exceptions-test, for each of the four concrete classes, their place in the hierarchy as an app's catch blocks
 * see it, the message, that the cause is null (AOSP hides it on purpose), and a round trip through a Parcel. Public API
 * only. tools/android-reference.sh takes the log from Android 15, and tools/service-exceptions-check.sh compares
 * Minato's with it.
 */
public class ServiceExceptionsApplication extends Application {
    private static final String TAG = "service-exceptions-test";

    @Override
    public void onCreate() {
        super.onCreate();
        check("foreground", new ForegroundServiceStartNotAllowedException("fg not allowed"),
                ForegroundServiceStartNotAllowedException.CREATOR);
        check("background", new BackgroundServiceStartNotAllowedException("bg not allowed"),
                BackgroundServiceStartNotAllowedException.CREATOR);
        check("missing", new MissingForegroundServiceTypeException("no type"),
                MissingForegroundServiceTypeException.CREATOR);
        check("invalid", new InvalidForegroundServiceTypeException("bad type"),
                InvalidForegroundServiceTypeException.CREATOR);
        try {
            throw new MissingForegroundServiceTypeException("thrown");
        } catch (ForegroundServiceTypeException e) {
            Log.i(TAG, "caught as ForegroundServiceTypeException " + e.getMessage());
        }
        try {
            throw new BackgroundServiceStartNotAllowedException("thrown");
        } catch (IllegalStateException e) {
            Log.i(TAG, "caught as IllegalStateException " + (e instanceof ServiceStartNotAllowedException));
        }
        Log.i(TAG, "done");
    }

    private <T extends Exception & Parcelable> void check(String name, T exception, Parcelable.Creator<T> creator) {
        Class<?> type = exception.getClass();
        Log.i(TAG, name + " " + type.getSimpleName() + " super=" + type.getSuperclass().getSimpleName()
                + " abstract=" + java.lang.reflect.Modifier.isAbstract(type.getModifiers())
                + " final=" + java.lang.reflect.Modifier.isFinal(type.getModifiers())
                + " state=" + (exception instanceof IllegalStateException)
                + " service=" + (exception instanceof ServiceStartNotAllowedException)
                + " type=" + (exception instanceof ForegroundServiceTypeException)
                + " msg=" + exception.getMessage() + " cause=" + exception.getCause()
                + " describe=" + ((Parcelable) exception).describeContents());
        Parcel parcel = Parcel.obtain();
        try {
            exception.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            T back = creator.createFromParcel(parcel);
            Log.i(TAG, name + " parcel " + back.getClass().getSimpleName() + " " + back.getMessage() + " "
                    + creator.newArray(3).length);
        } finally {
            parcel.recycle();
        }
    }
}
