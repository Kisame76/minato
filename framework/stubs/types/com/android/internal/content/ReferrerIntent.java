package com.android.internal.content;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Instrumentation and Activity's new intents carry the
 * referrer in it.
 */
public class ReferrerIntent extends android.content.Intent {

    public String mReferrer;
}
