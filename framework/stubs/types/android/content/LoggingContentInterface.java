package android.content;

import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import java.io.FileNotFoundException;
import java.util.ArrayList;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: a provider's transport logs through it on
 * debuggable builds when its tag is loggable at VERBOSE.
 */
public class LoggingContentInterface implements ContentInterface {
    public LoggingContentInterface(String tag, ContentInterface delegate) {
        throw new RuntimeException("stub");
    }

    public Cursor query(Uri uri, String[] projection, Bundle queryArgs, CancellationSignal cancellationSignal) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public String getType(Uri uri) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public String[] getStreamTypes(Uri uri, String mimeTypeFilter) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public Uri canonicalize(Uri uri) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public Uri uncanonicalize(Uri uri) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public boolean refresh(Uri uri, Bundle extras, CancellationSignal cancellationSignal) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public int checkUriPermission(Uri uri, int uid, int modeFlags) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public Uri insert(Uri uri, ContentValues initialValues, Bundle extras) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public int bulkInsert(Uri uri, ContentValues[] initialValues) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public int delete(Uri uri, Bundle extras) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public int update(Uri uri, ContentValues values, Bundle extras) throws RemoteException {
        throw new RuntimeException("stub");
    }

    public ParcelFileDescriptor openFile(Uri uri, String mode, CancellationSignal signal) throws RemoteException, FileNotFoundException {
        throw new RuntimeException("stub");
    }

    public AssetFileDescriptor openAssetFile(Uri uri, String mode, CancellationSignal signal) throws RemoteException, FileNotFoundException {
        throw new RuntimeException("stub");
    }

    public AssetFileDescriptor openTypedAssetFile(Uri uri, String mimeTypeFilter, Bundle opts, CancellationSignal signal) throws RemoteException, FileNotFoundException {
        throw new RuntimeException("stub");
    }

    public ContentProviderResult[] applyBatch(String authority, ArrayList<ContentProviderOperation> operations) throws RemoteException, OperationApplicationException {
        throw new RuntimeException("stub");
    }

    public Bundle call(String authority, String method, String arg, Bundle extras) throws RemoteException {
        throw new RuntimeException("stub");
    }
}
