package com.android.internal.util;

/**
 * Compile-only stub (ADR-0039 b3.4b), never in framework.jar: android.util.Xml, which ResourcesImpl takes for
 * asAttributeSet, makes one for binary XML, which no Minato process reads or writes yet. It stands on kxml2 of the
 * boot jars for the XmlPull methods, so that only the typed ones need a body here.
 */
public class ArtBinaryXmlPullParser extends com.android.modules.utils.BinaryXmlPullParser {
}
