package io.github.kisame76.minato.test.components;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.util.Log;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileWriter;
import java.io.IOException;

/** A provider of one root with one folder and one text file, made by the system when the app starts. */
public class DocsProvider extends DocumentsProvider {
    private static final String[] ROOT_COLUMNS = {Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE,
        Root.COLUMN_DOCUMENT_ID, Root.COLUMN_MIME_TYPES};
    private static final String[] DOC_COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_DISPLAY_NAME, Document.COLUMN_FLAGS, Document.COLUMN_SIZE};

    @Override
    public boolean onCreate() {
        Log.i(ComponentsActivity.TAG, "docsProvider onCreate");
        return true;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        cursor.newRow().add(Root.COLUMN_ROOT_ID, "root").add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY)
                .add(Root.COLUMN_TITLE, "Test root").add(Root.COLUMN_DOCUMENT_ID, "dir")
                .add(Root.COLUMN_MIME_TYPES, "*/*");
        return cursor;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        if ("dir".equals(documentId)) {
            cursor.newRow().add(Document.COLUMN_DOCUMENT_ID, "dir").add(Document.COLUMN_MIME_TYPE,
                    Document.MIME_TYPE_DIR).add(Document.COLUMN_DISPLAY_NAME, "Folder").add(Document.COLUMN_FLAGS, 0)
                    .add(Document.COLUMN_SIZE, 0);
        } else if ("dir/note.txt".equals(documentId)) {
            cursor.newRow().add(Document.COLUMN_DOCUMENT_ID, documentId).add(Document.COLUMN_MIME_TYPE, "text/plain")
                    .add(Document.COLUMN_DISPLAY_NAME, "note.txt").add(Document.COLUMN_FLAGS, 0)
                    .add(Document.COLUMN_SIZE, 5);
        } else {
            throw new FileNotFoundException(documentId);
        }
        return cursor;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        if ("dir".equals(parentDocumentId)) {
            cursor.newRow().add(Document.COLUMN_DOCUMENT_ID, "dir/note.txt").add(Document.COLUMN_MIME_TYPE,
                    "text/plain").add(Document.COLUMN_DISPLAY_NAME, "note.txt").add(Document.COLUMN_FLAGS, 0)
                    .add(Document.COLUMN_SIZE, 5);
        }
        return cursor;
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        File file = new File(getContext().getCacheDir(), "note.txt");
        try (FileWriter writer = new FileWriter(file)) {
            writer.write("hello");
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        return documentId.startsWith(parentDocumentId + "/");
    }
}
