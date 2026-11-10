package com.android.internal.minato.install;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;

/**
 * The certificate of an APK's first signer, read from its signing block (APK Signature Scheme v3, else v2) without
 * verifying anything: the installer's check that an update has the key of the app it replaces, the host verifies the
 * signature itself when it imports. Null for an APK without such a block (v1 only) or one that does not parse.
 */
final class ApkSigners {
    private static final long BLOCK_MAGIC_LOW = 0x20676953204b5041L;  // "APK Sig "
    private static final long BLOCK_MAGIC_HIGH = 0x3234206b636f6c42L; // "Block 42"
    private static final int V2_ID = 0x7109871a;
    private static final int V3_ID = 0xf05368c0;
    private static final int MAX_BLOCK = 16 * 1024 * 1024;

    private ApkSigners() {
    }

    /** The first signer's leaf certificate (DER), or null. */
    static byte[] leafCertificate(File apk) {
        try (RandomAccessFile file = new RandomAccessFile(apk, "r")) {
            final long length = file.length();
            // The end of central directory record: its last 22 bytes without a comment, found from the end.
            final int tail = (int) Math.min(length, 22 + 0xffff);
            final byte[] end = new byte[tail];
            file.seek(length - tail);
            file.readFully(end);
            int eocd = -1;
            for (int i = tail - 22; i >= 0; i--) {
                if (end[i] == 0x50 && end[i + 1] == 0x4b && end[i + 2] == 0x05 && end[i + 3] == 0x06) {
                    eocd = i;
                    break;
                }
            }
            if (eocd < 0) {
                return null;
            }
            final long directory = ByteBuffer.wrap(end, eocd + 16, 4).order(ByteOrder.LITTLE_ENDIAN).getInt()
                    & 0xffffffffL;
            if (directory < 32 || directory > length) {
                return null;
            }
            // The block ends at the central directory: its size (8), the pairs, its size again (8), the magic (16).
            final byte[] footer = new byte[24];
            file.seek(directory - 24);
            file.readFully(footer);
            final ByteBuffer foot = ByteBuffer.wrap(footer).order(ByteOrder.LITTLE_ENDIAN);
            if (foot.getLong(8) != BLOCK_MAGIC_LOW || foot.getLong(16) != BLOCK_MAGIC_HIGH) {
                return null;
            }
            final long size = foot.getLong(0);
            if (size < 24 || size > MAX_BLOCK || directory - size - 8 < 0) {
                return null;
            }
            final byte[] pairs = new byte[(int) (size - 24)];
            file.seek(directory - size);
            file.readFully(pairs);
            final byte[] v3 = firstCertificate(pairs, V3_ID);
            return v3 != null ? v3 : firstCertificate(pairs, V2_ID);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /** The first signer's first certificate in the pair `id` of the block's pairs, or null. */
    private static byte[] firstCertificate(byte[] pairs, int id) {
        final ByteBuffer all = ByteBuffer.wrap(pairs).order(ByteOrder.LITTLE_ENDIAN);
        while (all.remaining() >= 12) {
            final long length = all.getLong();
            if (length < 4 || length - 4 > all.remaining()) {
                return null;
            }
            final int pairId = all.getInt();
            final int valueLength = (int) (length - 4);
            if (pairId != id) {
                all.position(all.position() + valueLength);
                continue;
            }
            final ByteBuffer value = slice(all, valueLength);
            final ByteBuffer signers = lengthPrefixed(value);          // the sequence of signers
            final ByteBuffer signer = lengthPrefixed(signers);         // the first signer
            final ByteBuffer signed = lengthPrefixed(signer);          // its signed data
            lengthPrefixed(signed);                                    // the digests, skipped
            final ByteBuffer certificates = lengthPrefixed(signed);
            final ByteBuffer certificate = lengthPrefixed(certificates);
            final byte[] der = new byte[certificate.remaining()];
            certificate.get(der);
            return der;
        }
        return null;
    }

    private static ByteBuffer slice(ByteBuffer from, int length) {
        final ByteBuffer part = from.slice().order(ByteOrder.LITTLE_ENDIAN);
        part.limit(length);
        from.position(from.position() + length);
        return part;
    }

    /** The next uint32 length-prefixed part of `from`, which moves past it. */
    private static ByteBuffer lengthPrefixed(ByteBuffer from) {
        final int length = from.getInt();
        if (length < 0 || length > from.remaining()) {
            throw new IllegalArgumentException("bad length");
        }
        return slice(from, length);
    }
}
