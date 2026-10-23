/*
 * The documents provider of the primary external storage (ADR-0057 step b): Android's
 * com.android.externalstorage.ExternalStorageProvider, local in the app's process, which ActivityThread.acquireProvider
 * gives for the authority "com.android.externalstorage.documents" as system_server publishes Android's. It serves
 * /storage/emulated/0, where the host cloned the picks (Download), and answers as Android 15's does for what a picker
 * returns [verified: documents-test and `content query` on apk-runner-35 as root, with files pushed to Download]:
 *   - document ids are "primary:<path below /storage/emulated/0>", "primary:" the root itself, whose name is "0";
 *   - a row has the columns document_id, mime_type, _display_name, last_modified, flags, _size, in this order;
 *   - a file has the MIME type of its extension, application/octet-stream without a known one, and the flags
 *     FLAG_SUPPORTS_WRITE, DELETE, RENAME and MOVE (326); a folder vnd.android.document/directory and the flags
 *     FLAG_DIR_SUPPORTS_CREATE, DELETE, RENAME, MOVE, FLAG_SUPPORTS_METADATA and FLAG_DIR_BLOCKS_OPEN_DOCUMENT_TREE
 *     (49484);
 *   - a document that does not exist gives no cursor (FileNotFoundException, logged), and an id of another root, the
 *     same;
 *   - the app reaches a document it holds a grant for, and the SecurityException of ContentProvider's check without
 *     one (Android's check runs in the provider's process for a caller of another uid; here the app is its own
 *     caller, so the check is made at the document, with UriGrants).
 * Departures: no thumbnails, no metadata, no search, no recents, no copy and no Android/data or Android/obb restriction.
 * The folder's children come in the order the file system lists them. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles without android.*.
 */
package com.android.externalstorage;

import android.Manifest;
import android.content.AttributionSource;
import android.content.Intent;
import android.content.PermissionChecker;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.CancellationSignal;
import android.os.FileUtils;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Path;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import com.android.internal.minato.picker.ExternalStorage;
import com.android.internal.minato.picker.UriGrants;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** @hide */
public final class ExternalStorageProvider extends DocumentsProvider {
    public static final String AUTHORITY = DocumentsContract.EXTERNAL_STORAGE_PROVIDER_AUTHORITY;
    private static final String ROOT_ID_PRIMARY = "primary";
    private static final String MIME_OCTET_STREAM = "application/octet-stream";

    private static final String[] DEFAULT_ROOT_PROJECTION = {Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_ICON,
        Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_AVAILABLE_BYTES, Root.COLUMN_MIME_TYPES};
    private static final String[] DEFAULT_DOCUMENT_PROJECTION = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_DISPLAY_NAME, Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS, Document.COLUMN_SIZE};

    /** The flags of a file and of a folder: see the comment above. */
    private static final int FILE_FLAGS = Document.FLAG_SUPPORTS_WRITE | Document.FLAG_SUPPORTS_DELETE
            | Document.FLAG_SUPPORTS_RENAME | Document.FLAG_SUPPORTS_MOVE;
    private static final int FOLDER_FLAGS = Document.FLAG_DIR_SUPPORTS_CREATE | Document.FLAG_SUPPORTS_DELETE
            | Document.FLAG_SUPPORTS_RENAME | Document.FLAG_SUPPORTS_MOVE | Document.FLAG_SUPPORTS_METADATA
            | Document.FLAG_DIR_BLOCKS_OPEN_DOCUMENT_TREE;
    /** Root.FLAG_SUPPORTS_CREATE, LOCAL_ONLY, SUPPORTS_SEARCH and SUPPORTS_IS_CHILD, as the root of the AVD has them. */
    private static final int ROOT_FLAGS = Root.FLAG_SUPPORTS_CREATE | Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_SEARCH
            | Root.FLAG_SUPPORTS_IS_CHILD | Root.FLAG_ADVANCED;

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public void attachInfo(android.content.Context context, android.content.pm.ProviderInfo info) {
        // The provider as Android's manifest declares it, which DocumentsProvider checks.
        info.exported = true;
        info.grantUriPermissions = true;
        info.readPermission = Manifest.permission.MANAGE_DOCUMENTS;
        info.writePermission = Manifest.permission.MANAGE_DOCUMENTS;
        super.attachInfo(context, info);
    }

    /** What the package manager lists for the intent DocumentsProvider.PROVIDER_INTERFACE: this provider. */
    public static android.content.pm.ResolveInfo resolveInfo(android.content.Context context) {
        final android.content.pm.ResolveInfo resolved = new android.content.pm.ResolveInfo();
        resolved.providerInfo = providerInfo(context);
        resolved.match = 0x108000;
        return resolved;
    }

    /** The ProviderInfo of this provider for ActivityThread. */
    public static android.content.pm.ProviderInfo providerInfo(android.content.Context context) {
        final android.content.pm.ProviderInfo info = new android.content.pm.ProviderInfo();
        info.name = ExternalStorageProvider.class.getName();
        info.authority = AUTHORITY;
        info.packageName = "com.android.externalstorage";
        info.applicationInfo = context.getApplicationInfo();
        return info;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        final MatrixCursor result = new MatrixCursor(projection != null ? projection : DEFAULT_ROOT_PROJECTION);
        final MatrixCursor.RowBuilder row = result.newRow();
        row.add(Root.COLUMN_ROOT_ID, ROOT_ID_PRIMARY);
        row.add(Root.COLUMN_FLAGS, ROOT_FLAGS);
        row.add(Root.COLUMN_TITLE, Build.MODEL);
        row.add(Root.COLUMN_DOCUMENT_ID, ROOT_ID_PRIMARY + ":");
        row.add(Root.COLUMN_AVAILABLE_BYTES, -1L);
        return result;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        final File file = fileFor(documentId);
        final MatrixCursor result = new MatrixCursor(projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION);
        include(result, documentId, file);
        return result;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        final File parent = fileFor(parentDocumentId);
        final MatrixCursor result = new MatrixCursor(projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION);
        final File[] children = parent.listFiles();
        if (children != null) {
            for (final File child : children) {
                include(result, childId(parentDocumentId, child.getName()), child);
            }
        }
        return result;
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        try {
            final String parent = fileFor(parentDocumentId).getCanonicalPath();
            final String child = fileFor(documentId).getCanonicalPath();
            return child.equals(parent) || child.startsWith(parent.endsWith("/") ? parent : parent + "/");
        } catch (IOException e) {
            return false;
        }
    }

    @Override
    public String getDocumentType(String documentId) throws FileNotFoundException {
        return mimeTypeOf(fileFor(documentId));
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        final File file = fileFor(documentId);
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode));
    }

    /**
     * ContentProvider's check of a read, which Android's runs in the provider's process against the caller's grants;
     * the app is its own caller here, which ContentProvider lets through, so the grants are asked at the URI (a
     * document's, or a document below a tree) for every query, open and call.
     */
    @Override
    protected int enforceReadPermissionInner(Uri uri, AttributionSource attributionSource) {
        return enforce(uri, false);
    }

    @Override
    protected int enforceWritePermissionInner(Uri uri, AttributionSource attributionSource) {
        return enforce(uri, true);
    }

    /** PERMISSION_GRANTED, or the SecurityException of ContentProvider's check without a grant. */
    private int enforce(Uri uri, boolean write) {
        final int mode = write ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : Intent.FLAG_GRANT_READ_URI_PERMISSION;
        if (!UriGrants.allows(uri, mode)) {
            throw new SecurityException("Permission Denial: " + (write ? "writing " : "reading ")
                    + ExternalStorageProvider.class.getName() + " uri " + uri + " from pid=" + Binder.getCallingPid()
                    + ", uid=" + Binder.getCallingUid() + " requires that you obtain access using ACTION_OPEN_DOCUMENT "
                    + "or related APIs");
        }
        return PermissionChecker.PERMISSION_GRANTED;
    }

    /** The document id of a file below the storage's root: "primary:<path>". */
    private static String docIdFor(File file) throws FileNotFoundException {
        try {
            final String root = new File(ExternalStorage.ROOT).getCanonicalPath();
            final String path = file.getCanonicalPath();
            if (path.equals(root)) {
                return ROOT_ID_PRIMARY + ":";
            }
            if (!path.startsWith(root + "/")) {
                throw new FileNotFoundException("Document " + file + " is outside the storage");
            }
            return ROOT_ID_PRIMARY + ":" + path.substring(root.length() + 1);
        } catch (IOException e) {
            throw new FileNotFoundException("Missing file for " + file);
        }
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName)
            throws FileNotFoundException {
        final File parent = fileFor(parentDocumentId);
        if (!parent.isDirectory()) {
            throw new IllegalArgumentException("Parent document isn't a directory");
        }
        // As Android's provider does for a name the app gave: no "/", no ".." and the characters a FAT name has.
        displayName = FileUtils.buildValidFatFilename(displayName);
        final File file;
        if (Document.MIME_TYPE_DIR.equals(mimeType)) {
            file = FileUtils.buildUniqueFile(parent, displayName);
            if (!file.mkdir()) {
                throw new IllegalStateException("Failed to mkdir " + file);
            }
        } else {
            file = FileUtils.buildUniqueFile(parent, mimeType, displayName);
            try {
                if (!file.createNewFile()) {
                    throw new IllegalStateException("Failed to touch " + file);
                }
            } catch (IOException e) {
                throw new IllegalStateException("Failed to touch " + file + ": " + e);
            }
        }
        return docIdFor(file);
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        final File file = fileFor(documentId);
        if (file.equals(new File(ExternalStorage.ROOT).getAbsoluteFile())) {
            throw new IllegalStateException("Failed to delete " + file);
        }
        try {
            deleteTree(file.toPath());
        } catch (IOException e) {
            throw new IllegalStateException("Failed to delete " + file + ": " + e);
        }
    }

    /** Deletes a file or a folder with what is in it, a link as the link: none is followed. */
    private static void deleteTree(java.nio.file.Path path) throws IOException {
        if (Files.isDirectory(path, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
            try (java.util.stream.Stream<java.nio.file.Path> children = Files.list(path)) {
                for (final java.nio.file.Path child : (Iterable<java.nio.file.Path>) children::iterator) {
                    deleteTree(child);
                }
            }
        }
        Files.delete(path);
    }

    @Override
    public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        final File before = fileFor(documentId);
        final File after = FileUtils.buildUniqueFile(before.getParentFile(),
                FileUtils.buildValidFatFilename(displayName));
        if (!before.renameTo(after)) {
            throw new IllegalStateException("Failed to rename to " + after);
        }
        final String afterDocId = docIdFor(after);
        return afterDocId.equals(documentId) ? null : afterDocId;
    }

    @Override
    public String moveDocument(String sourceDocumentId, String sourceParentDocumentId, String targetParentDocumentId)
            throws FileNotFoundException {
        final File before = fileFor(sourceDocumentId);
        final File target = fileFor(targetParentDocumentId);
        final File after = FileUtils.buildUniqueFile(target, mimeTypeOf(before),
                FileUtils.buildValidFatFilename(before.getName()));
        if (!before.renameTo(after)) {
            throw new IllegalStateException("Failed to move to " + after);
        }
        return docIdFor(after);
    }

    @Override
    public Path findDocumentPath(String parentDocumentId, String childDocumentId) throws FileNotFoundException {
        final File child = fileFor(childDocumentId);
        final List<String> ids = new ArrayList<>();
        final File root = new File(ExternalStorage.ROOT);
        final File stop = parentDocumentId != null ? fileFor(parentDocumentId) : root;
        for (File file = child; file != null; file = file.getParentFile()) {
            ids.add(0, docIdFor(file));
            if (file.equals(stop) || file.equals(root)) {
                break;
            }
        }
        return new Path(parentDocumentId == null ? ROOT_ID_PRIMARY : null, ids);
    }

    /** The file of a document id, FileNotFoundException for another root, a path out of the storage, or none. */
    private File fileFor(String documentId) throws FileNotFoundException {
        final int split = documentId.indexOf(':', 1);
        if (split < 0 || !documentId.substring(0, split).equals(ROOT_ID_PRIMARY)) {
            throw new FileNotFoundException("No root for " + documentId);
        }
        final File root = new File(ExternalStorage.ROOT);
        final File file = new File(root, documentId.substring(split + 1));
        try {
            final String canonical = file.getCanonicalPath();
            if (!canonical.equals(root.getCanonicalPath()) && !canonical.startsWith(root.getCanonicalPath() + "/")) {
                throw new FileNotFoundException("Document " + documentId + " is outside the storage");
            }
        } catch (IOException e) {
            throw new FileNotFoundException("Missing file for " + documentId);
        }
        if (!file.exists()) {
            throw new FileNotFoundException("Missing file for " + documentId + " at " + file);
        }
        // The canonical file, so that a link swapped in after the check does not move the operation elsewhere.
        try {
            return file.getCanonicalFile();
        } catch (IOException e) {
            throw new FileNotFoundException("Missing file for " + documentId);
        }
    }

    private static String childId(String parentDocumentId, String name) {
        return parentDocumentId.endsWith(":") || parentDocumentId.endsWith("/") ? parentDocumentId + name
                : parentDocumentId + "/" + name;
    }

    private static String mimeTypeOf(File file) {
        if (file.isDirectory()) {
            return Document.MIME_TYPE_DIR;
        }
        final String name = file.getName();
        final int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            final String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1)
                    .toLowerCase());
            if (type != null) {
                return type;
            }
        }
        return MIME_OCTET_STREAM;
    }

    /** The flags of a folder; the storage's root and Download are the ones DocumentsUI refuses to open as a tree. */
    private static int folderFlags(File folder) {
        final String path = folder.getPath();
        final boolean blocked = path.equals(ExternalStorage.ROOT) || path.equals(ExternalStorage.DOWNLOAD);
        return blocked ? FOLDER_FLAGS : FOLDER_FLAGS & ~Document.FLAG_DIR_BLOCKS_OPEN_DOCUMENT_TREE;
    }

    private static void include(MatrixCursor result, String documentId, File file) {
        final MatrixCursor.RowBuilder row = result.newRow();
        row.add(Document.COLUMN_DOCUMENT_ID, documentId);
        row.add(Document.COLUMN_MIME_TYPE, mimeTypeOf(file));
        row.add(Document.COLUMN_DISPLAY_NAME, file.getName());
        row.add(Document.COLUMN_LAST_MODIFIED, file.lastModified());
        row.add(Document.COLUMN_FLAGS, file.isDirectory() ? folderFlags(file) : FILE_FLAGS);
        // A folder's length is its file system's, 4096 on the AVD's ext4, where the host's differs.
        row.add(Document.COLUMN_SIZE, file.isDirectory() ? 4096L : file.length());
    }
}
