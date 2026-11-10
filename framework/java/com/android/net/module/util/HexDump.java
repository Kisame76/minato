package com.android.net.module.util;

/**
 * The hexadecimal text of bytes, for the connectivity module's classes that name it (NsdServiceInfo's toString). Ours:
 * the module's own is not among the fetched sources.
 */
public final class HexDump {
    private static final char[] DIGITS = "0123456789ABCDEF".toCharArray();

    private HexDump() {
    }

    public static String toHexString(byte[] array) {
        return toHexString(array, 0, array.length);
    }

    public static String toHexString(byte[] array, int offset, int length) {
        final char[] result = new char[length * 2];
        for (int i = 0; i < length; i++) {
            final int b = array[offset + i] & 0xff;
            result[i * 2] = DIGITS[b >>> 4];
            result[i * 2 + 1] = DIGITS[b & 0xf];
        }
        return new String(result);
    }
}
