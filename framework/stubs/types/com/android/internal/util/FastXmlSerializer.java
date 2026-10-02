package com.android.internal.util;

import com.android.org.kxml2.io.KXmlSerializer;

/**
 * Compile-only stub (ADR-0039 b3.4b), never in framework.jar: android.util.Xml, which ResourcesImpl takes for
 * asAttributeSet, makes one to write XML, which no Minato process does yet. It stands on kxml2 of the boot jars for
 * the XmlSerializer methods.
 */
public class FastXmlSerializer extends KXmlSerializer {
}
