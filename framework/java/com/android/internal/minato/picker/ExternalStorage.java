package com.android.internal.minato.picker;

/** The places of the documents provider (ADR-0057 step b): Android's primary external storage, as the guest sees it. */
public final class ExternalStorage {
    /** Environment.getExternalStorageDirectory() of the owner, which the root "primary" names. */
    public static final String ROOT = "/storage/emulated/0";
    /** The folder the host clones picks into, and the documents "primary:Download/<name>" name. */
    public static final String DOWNLOAD = ROOT + "/Download";

    private ExternalStorage() {
    }
}
