package com.android.minato.picker;

import java.util.List;

/**
 * The URI grants of an app on the host's JVM (tools/framework-host-check.sh), as Android's UriGrantsManagerService
 * keeps them for the app: a picker's result grants the document, the app may keep the grant with
 * takePersistableUriPermission when the result carried FLAG_GRANT_PERSISTABLE_URI_PERMISSION, and only a kept grant
 * outlives the app's process; the table's text is what the framework stores.
 */
public final class GrantTableTest {
    private static int failures;
    private static final int READ = GrantTable.READ, WRITE = GrantTable.WRITE, PERSIST = GrantTable.PERSISTABLE;
    private static final String DOC = "content://a.b/document/x%3A1";
    private static final String PREFIX_OK = "content://a.b/document/";

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    private static boolean refuses(Runnable action) {
        try {
            action.run();
            return false;
        } catch (SecurityException e) {
            return true;
        }
    }

    public static void main(String[] args) {
        final GrantTable table = new GrantTable();
        check(!table.allows(DOC, READ), "no grant, no access");
        table.grant(DOC, READ | WRITE | PERSIST);
        check(table.allows(DOC, READ) && table.allows(DOC, WRITE) && table.allows(DOC, READ | WRITE), "granted modes");
        check(!table.allows(DOC + "2", READ), "another URI is not granted");
        check(!table.allows("content://a.b/document/x%3A", READ), "a prefix of the URI is not the URI");
        check(!table.allows(DOC, 0), "a mode of 0 is never granted");
        check(table.persisted().isEmpty(), "a grant is not kept until the app takes it");
        check(refuses(() -> table.take(DOC + "2", READ, 5)), "taking a URI without a grant is refused");
        check(refuses(() -> new GrantTable().take(DOC, READ, 5)), "taking without any grant is refused");
        final GrantTable plain = new GrantTable();
        plain.grant(DOC, READ);
        check(refuses(() -> plain.take(DOC, READ, 5)), "taking a grant without FLAG_GRANT_PERSISTABLE is refused");
        final GrantTable readOnly = new GrantTable();
        readOnly.grant(DOC, READ | PERSIST);
        check(refuses(() -> readOnly.take(DOC, WRITE, 5)), "taking a mode the grant lacks is refused");
        check(refuses(() -> readOnly.take(DOC, READ | WRITE, 5)), "taking more than the grant is refused");
        table.take(DOC, READ, 7);
        List<GrantTable.Entry> kept = table.persisted();
        check(kept.size() == 1 && kept.get(0).uri.equals(DOC) && kept.get(0).modes == READ
                && kept.get(0).time == 7, "a kept grant has the modes taken and the time");
        table.take(DOC, READ, 9);
        check(table.persisted().get(0).time == 7, "taking again keeps the first time");
        table.take(DOC, WRITE, 11);
        check(table.persisted().get(0).modes == (READ | WRITE), "taking another mode adds it");
        final String text = table.serialize();
        final GrantTable again = GrantTable.parse(text, PREFIX_OK);
        check(again.persisted().size() == 1 && again.persisted().get(0).modes == (READ | WRITE)
                && again.persisted().get(0).time == 7, "a kept grant survives the text");
        check(again.allows(DOC, READ | WRITE), "and still grants");
        check(!again.allows(DOC, 0x80), "and grants nothing more");
        table.release(DOC, WRITE);
        check(table.persisted().get(0).modes == READ, "releasing a mode drops it from the kept grant");
        check(table.allows(DOC, WRITE), "and leaves the grant of this run");
        table.release(DOC, READ);
        check(table.persisted().isEmpty(), "releasing the last mode drops the kept grant");
        table.release(DOC, READ);
        table.release(DOC + "2", READ);
        check(table.persisted().isEmpty(), "releasing what is not kept does nothing");
        check(GrantTable.parse("", PREFIX_OK).persisted().isEmpty(), "an empty text has no grants");
        for (final String bad : new String[] {"x", "1\t2", "a\tb\tc", "1\t0\t" + DOC, "-1\t1\t" + DOC, "1\t9\t" + DOC,
            "1\t1\t", "1\t1\tnot a uri", "x\t1\t" + DOC, "1\tx\t" + DOC, "1\t1\t" + DOC + "\textra"}) {
            check(GrantTable.parse(bad + "\n", PREFIX_OK).persisted().isEmpty(), "a bad line is dropped: " + bad);
        }
        final GrantTable mixed = GrantTable.parse("garbage\n3\t1\t" + DOC + "\n\n4\t3\t" + DOC + "2\n", PREFIX_OK);
        check(mixed.persisted().size() == 2, "good lines survive bad ones");
        check(GrantTable.parse("1\t1\t" + DOC + "\n2\t1\t" + DOC + "\n", PREFIX_OK).persisted().size() == 1,
                "a URI once");
        check(GrantTable.parse("1\t3\tcontent://other/document/x\n", PREFIX_OK).persisted().isEmpty(),
                "a URI of another authority is dropped: the file is the app's to write");
        check(GrantTable.parse("1\t3\tcontent://a.b/tree/x\n", PREFIX_OK).persisted().isEmpty(),
                "and one that is no document");
        final GrantTable folder = new GrantTable();
        folder.grant("content://a.b/tree/x%3A1", READ | WRITE | PERSIST | GrantTable.PREFIX);
        folder.take("content://a.b/tree/x%3A1", READ | WRITE, 5);
        final GrantTable folderAgain = GrantTable.parse(folder.serialize(), "content://a.b/");
        check(folderAgain.persisted().size() == 1 && folderAgain.persisted().get(0).prefix
                && folderAgain.allows("content://a.b/tree/x%3A1/document/x%3A1%2Fy", READ | WRITE),
                "a kept folder grant still covers what is below it after the text");
        check(GrantTable.parse("5\t4\tcontent://a.b/tree/x\n", "content://a.b/").persisted().isEmpty(),
                "stored modes of only the prefix bit are dropped");
        final GrantTable prefix = new GrantTable();
        prefix.grant("content://a.b/tree/x%3A1", READ | GrantTable.PREFIX);
        check(prefix.allows("content://a.b/tree/x%3A1", READ), "a prefix grant covers its own URI");
        check(prefix.allows("content://a.b/tree/x%3A1/document/x%3A1%2Fy", READ), "and what is below it");
        check(!prefix.allows("content://a.b/tree/x%3A12", READ), "and not a longer name");
        if (failures != 0) {
            System.out.println(failures + " failures");
            System.exit(1);
        }
        System.out.println("GrantTableTest ok");
    }
}
