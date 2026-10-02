package com.android.modules.utils;

import com.android.org.kxml2.io.KXmlParser;

/**
 * Compile-only stub (ADR-0039 b3.4b), never in framework.jar: android.util.Xml, which ResourcesImpl takes for
 * asAttributeSet, makes one for binary XML, which no Minato process reads or writes yet. It stands on kxml2 of the
 * boot jars for the XmlPull methods, so that only the typed ones need a body here.
 */
public class BinaryXmlPullParser extends KXmlParser implements TypedXmlPullParser {
    @Override
    public byte[] getAttributeBytesHex(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public byte[] getAttributeBytesBase64(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public int getAttributeInt(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public int getAttributeIntHex(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public long getAttributeLong(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public long getAttributeLongHex(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public float getAttributeFloat(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public double getAttributeDouble(int index) {
        throw new RuntimeException("stub");
    }

    @Override
    public boolean getAttributeBoolean(int index) {
        throw new RuntimeException("stub");
    }
}
