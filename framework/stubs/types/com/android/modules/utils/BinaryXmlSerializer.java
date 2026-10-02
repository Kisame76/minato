package com.android.modules.utils;

import com.android.org.kxml2.io.KXmlSerializer;
import org.xmlpull.v1.XmlSerializer;

/**
 * Compile-only stub (ADR-0039 b3.4b), never in framework.jar: android.util.Xml, which ResourcesImpl takes for
 * asAttributeSet, makes one for binary XML, which no Minato process reads or writes yet. It stands on kxml2 of the
 * boot jars for the XmlPull methods, so that only the typed ones need a body here.
 */
public class BinaryXmlSerializer extends KXmlSerializer implements TypedXmlSerializer {
    public static final byte[] PROTOCOL_MAGIC_VERSION_0 = new byte[] {0x41, 0x42, 0x58, 0x00};

    @Override
    public XmlSerializer attributeInterned(String namespace, String name, String value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeBytesHex(String namespace, String name, byte[] value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeBytesBase64(String namespace, String name, byte[] value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeInt(String namespace, String name, int value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeIntHex(String namespace, String name, int value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeLong(String namespace, String name, long value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeLongHex(String namespace, String name, long value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeFloat(String namespace, String name, float value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeDouble(String namespace, String name, double value) {
        throw new RuntimeException("stub");
    }

    @Override
    public XmlSerializer attributeBoolean(String namespace, String name, boolean value) {
        throw new RuntimeException("stub");
    }
}
