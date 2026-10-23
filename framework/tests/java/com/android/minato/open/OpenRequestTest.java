package com.android.minato.open;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The text of an open request on the host's JVM (tools/framework-host-check.sh): whatever an app puts into its intent,
 * the text stays inside the limits that the runtime enforces (guest_interface.h, ADR-0062), so the runtime does not
 * refuse the request of an app that did nothing wrong, and a hostile value never reaches the host as it is.
 */
public final class OpenRequestTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    /** `field` percent-decoded, or null for a bad escape or bytes that are no UTF-8. */
    private static String decode(String field) {
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        for (int at = 0; at < field.length(); at++) {
            final char c = field.charAt(at);
            if (c == '%') {
                if (at + 2 >= field.length() + 0 && at + 2 > field.length() - 1 + 0 && at + 2 >= field.length()) {
                    return null;
                }
                final int high = Character.digit(field.charAt(at + 1), 16);
                final int low = Character.digit(field.charAt(at + 2), 16);
                if (high < 0 || low < 0) {
                    return null;
                }
                final int value = high * 16 + low;
                if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')
                        || "-._~".indexOf(value) >= 0) {
                    return null;  // canonical encoding: an unreserved byte is raw
                }
                bytes.write(value);
                at += 2;
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || "-._~".indexOf(c) >= 0) {
                bytes.write(c);
            } else {
                return null;  // a byte that is not unreserved must be escaped
            }
        }
        try {
            return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes.toByteArray()))
                    .toString();
        } catch (CharacterCodingException e) {
            return null;
        }
    }

    private static int size(String text) {
        return text.getBytes(StandardCharsets.UTF_8).length;
    }

    /** The runtime's rules for the text, written again: the fields of the kind, each inside its limits. */
    private static boolean wellFormed(String text) {
        if (size(text) > 204800) {
            return false;
        }
        final String[] field = text.split("\t", -1);
        if (!field[0].matches("[1-4]")) {
            return false;
        }
        final int kind = field[0].charAt(0) - '0';
        final List<String> value = new ArrayList<>();
        for (int at = 1; at < field.length; at++) {
            final String decoded = decode(field[at]);
            if (decoded == null) {
                return false;
            }
            value.add(decoded);
        }
        if (kind <= 3) {
            if (value.size() != 1) {
                return false;
            }
            final String url = value.get(0);
            final int colon = url.indexOf(':');
            if (colon <= 0 || size(url) > 4096 || OpenRequest.hasControl(url) || url.chars().anyMatch(c ->
                    c == ' ' || Character.getType(c) == Character.SPACE_SEPARATOR || Character.getType(c) == Character.FORMAT)) {
                return false;
            }
            final String scheme = url.substring(0, colon).toLowerCase();
            return kind == 1 ? Arrays.asList("http", "https", "tel", "maps").contains(scheme)
                    : kind == 2 ? scheme.equals("mailto") : scheme.equals("sms");
        }
        if (value.size() < 3 || !value.get(2).matches("0|[1-9][0-9]*")) {
            return false;
        }
        final int count = Integer.parseInt(value.get(2));
        if (count > 10 || value.size() != 3 + 2 * count || size(value.get(0)) > 1000 || size(value.get(1)) > 65536) {
            return false;
        }
        final List<String> names = new ArrayList<>();
        for (int at = 0; at < count; at++) {
            final String name = value.get(3 + at);
            if (!OpenRequest.plainName(name) || names.contains(name)) {
                return false;
            }
            names.add(name);
            if (!OpenRequest.mimeType(value.get(3 + count + at))) {
                return false;
            }
        }
        return true;
    }

    /** Ten distinct names of 80 characters of 3 bytes each: as long as the names get. */
    private static List<String> distinctNames() {
        final List<String> names = new ArrayList<>();
        for (int at = 0; at < 10; at++) {
            names.add("€".repeat(79) + at);
        }
        return names;
    }

    private static String urlOf(String text) {
        return decode(text.split("\t", -1)[1]);
    }

    public static void main(String[] args) {
        // VIEW
        check(OpenRequest.view("https://example.com/a?b=c#d").equals("1\thttps%3A%2F%2Fexample.com%2Fa%3Fb%3Dc%23d"),
                "an https URL is one encoded field");
        for (String good : new String[] {"http://example.com", "HTTPS://user:pw@example.com:8080/x", "tel:+4930-1234;1",
                "tel:*100#", "maps://?q=Berlin", "https://bücher.example/straße?q=ü"}) {
            final String text = OpenRequest.view(good);
            check(text != null && wellFormed(text) && urlOf(text).equals(good), "view accepts " + good);
        }
        for (String bad : new String[] {"", "example.com", "ftp://example.com", "file:///etc/passwd", "javascript:alert(1)",
                "data:text/html,x", "intent://x#Intent;end", "market://details?id=a", "mailto:a@b.c", "sms:123",
                "http:", "http://", "http:///x", "https://?x", "http:example.com", "https://@/x", "tel:", "tel:abc",
                "tel:" + "1".repeat(65), "https://example.com/\u0000", "https://a/\u2028b",
                "https://example.com/" + "a".repeat(4096)}) {
            check(OpenRequest.view(bad) == null, "view refuses " + (bad.length() > 40 ? bad.substring(0, 40) : bad).replace("\n", "\\n"));
        }
        check(OpenRequest.view(null) == null, "view refuses null");
        check(OpenRequest.plainPoint('.') && OpenRequest.plainPoint(' ') && OpenRequest.plainPoint('\u20ac'),
              "a dot, a space and a euro sign may be in a name");
        check(!OpenRequest.plainPoint('/') && !OpenRequest.plainPoint(0) && !OpenRequest.plainPoint('\u200b')
              && !OpenRequest.plainPoint('\u00a0') && !OpenRequest.plainPoint(0xd800), "a slash, NUL, Cf, Zs and a surrogate may not");
        check(OpenRequest.view("https://example.com/" + "a".repeat(4096 - 20)) != null, "a URL of exactly 4096 bytes passes");
        check(OpenRequest.view("https://example.com/" + "a".repeat(4096 - 19)) == null, "a URL of 4097 bytes is refused");

        check(urlOf(OpenRequest.view("tel:+49 30 1234")).equals("tel:+49301234"), "the spaces of a tel number are taken out");
        check(urlOf(OpenRequest.view("https://example.com/a b?q=x y")).equals("https://example.com/a%20b?q=x%20y"),
                "a space in an http URL is %20");
        check(urlOf(OpenRequest.view("https://example.com/a\u00a0b\u200bc\u3000d")).equals(
                "https://example.com/a%C2%A0b%E2%80%8Bc%E3%80%80d"), "no-break, zero-width and ideographic spaces are escaped");
        check(OpenRequest.view("https://example.com/" + "a ".repeat(1400)) == null, "the escaped URL must fit 4096 bytes");

        // MAIL
        String mail = OpenRequest.mail(new String[] {"a@example.com", "b+c@example.com"}, new String[] {"cc@example.com"},
                new String[] {"bcc@example.com"}, "Hi & bye = ?", "line1\nline2 100% ü");
        check(mail != null && wellFormed(mail), "a mail request is well formed");
        check(mail != null && urlOf(mail).equals("mailto:a@example.com,b+c@example.com?cc=cc@example.com"
                + "&bcc=bcc@example.com&subject=Hi%20%26%20bye%20%3D%20%3F&body=line1%0Aline2%20100%25%20%C3%BC"),
                "the mailto: URL is RFC 6068's: " + (mail == null ? "null" : urlOf(mail)));
        check(urlOf(OpenRequest.mail(null, null, null, null, null)).equals("mailto:"), "an empty mail is mailto:");
        check(urlOf(OpenRequest.mail(new String[] {"a@b.c"}, null, null, "", "")).equals("mailto:a@b.c"),
                "empty parts are left out");
        final String hostile = OpenRequest.mail(new String[] {"x@y.z?bcc=evil@e.com&body=\r\n"}, null, null, null, null);
        check(hostile != null && wellFormed(hostile) && !urlOf(hostile).contains("?bcc=") && !urlOf(hostile).contains("\n"),
                "an address cannot add a parameter or a line");
        final String big = OpenRequest.mail(new String[] {"a@b.c"}, null, null, "s", "€".repeat(100000));
        check(big != null && wellFormed(big) && urlOf(big).startsWith("mailto:a@b.c?subject=s&body="),
                "a huge body is cut to fit the URL");
        check(big != null && !urlOf(big).contains("%E2%82%AC%E2%82") || big != null && urlOf(big).endsWith("%E2%82%AC")
                || big != null && urlOf(big).matches(".*(%E2%82%AC)+$"), "the body is cut between characters");
        final String longAddress = OpenRequest.mail(new String[] {"a".repeat(5000) + "@b.c"}, null, null, null, null);
        check(longAddress != null && wellFormed(longAddress) && size(urlOf(longAddress)) <= 600, "an address is cut at 500 bytes");
        final String[] manyAddresses = new String[200];
        Arrays.fill(manyAddresses, "x@y.z");
        check(urlOf(OpenRequest.mail(manyAddresses, null, null, null, null)).split(",").length == 50, "at most 50 addresses of a list");
        check(urlOf(OpenRequest.mail(new String[] {"", "  ", "a@b.c", null}, null, null, null, null)).equals("mailto:a@b.c"),
                "empty addresses are left out, no stray commas");
        check(OpenRequest.mail(new String[] {"a@b.c"}, null, null, null, "x".repeat(50_000_000)) != null, "a 50 MB body is cut fast");

        check(OpenRequest.view("tel:%23%2306%23") == null, "no escapes in a tel number");
        check(urlOf(OpenRequest.view("  https://example.com/\n x\t ")).equals("https://example.com/%20x"),
                "a link is read as a browser reads it: the ends stripped, tabs and line breaks inside taken out");
        check(OpenRequest.view("https://exa\u0000mple.com/") == null, "a NUL in a link is refused");

        // SMS
        check(urlOf(OpenRequest.sms("+49 (30) 123-456", "Hello & bye")).equals("sms:+49030123456".replace("49030", "4930")
                + "&body=Hello%20%26%20bye"), "an sms URL: " + urlOf(OpenRequest.sms("+49 (30) 123-456", "Hello & bye")));
        check(urlOf(OpenRequest.sms("123", null)).equals("sms:123"), "no body, no &body");
        check(urlOf(OpenRequest.sms(null, "hi")).equals("sms:&body=hi"), "no number is allowed");
        check(OpenRequest.sms("12a34", "x") == null, "a number with a letter is refused");
        check(OpenRequest.sms("1".repeat(65), "x") == null, "a number over 64 is refused");
        final String longSms = OpenRequest.sms("123", "ü".repeat(10000));
        check(longSms != null && wellFormed(longSms), "a huge sms body is cut to fit");

        // SHARE
        String share = OpenRequest.share("Sub", "Text", List.of("a.png", "b.txt"), List.of("image/png", "text/plain"));
        check(share != null && wellFormed(share) && share.equals("4\tSub\tText\t2\ta.png\tb.txt\timage%2Fpng\ttext%2Fplain"),
                "a share with two files: " + share);
        share = OpenRequest.share(null, null, null, null);
        check(share.equals("4\t\t\t0") && wellFormed(share), "a share of nothing");
        final List<String> many = new ArrayList<>();
        final List<String> types = new ArrayList<>();
        for (int at = 0; at < 15; at++) {
            many.add("f" + at);
            types.add("a/b");
        }
        share = OpenRequest.share("s", "t", many, types);
        check(wellFormed(share) && share.split("\t", -1)[3].equals("10"), "at most ten files");
        share = OpenRequest.share("s", "t", Arrays.asList("ok", "../x", "a/b", ".", "..", "", "ok", "tab\tname", "nul\u0000",
                "new\nline", "zero​width", "x".repeat(256), null, "fine ü"),
                Collections.nCopies(14, "text/plain"));
        check(wellFormed(share) && share.split("\t", -1)[3].equals("2"), "only the plain, new names stay: " + share);
        share = OpenRequest.share("s", "t", List.of("a", "b"), Arrays.asList("not a type", null));
        check(wellFormed(share) && share.endsWith("application%2Foctet-stream\tapplication%2Foctet-stream"),
                "a missing or bad type becomes application/octet-stream");
        share = OpenRequest.share("s", "t", List.of("a", "b"), List.of("text/plain"));
        check(wellFormed(share) && share.endsWith("text%2Fplain\tapplication%2Foctet-stream"), "a missing type, by position");
        final String worst = OpenRequest.share("€".repeat(2000), "€".repeat(70000), distinctNames(), Collections.nCopies(10, "a/b"));
        check(wellFormed(worst) && size(worst) <= 204800, "the worst case fits the whole: " + size(worst));
        check(decode(worst.split("\t", -1)[1]).equals("€".repeat(333)), "the subject is cut between characters at 1000 bytes");
        final String text = decode(OpenRequest.share("", "€".repeat(70000), null, null).split("\t", -1)[2]);
        check(text.equals("€".repeat(21845)), "the text is cut between characters at 65536 bytes: " + text.length());
        check(decode(OpenRequest.share("", "a".repeat(70000), null, null).split("\t", -1)[2]).length() == 65536,
                "an ASCII text is cut at exactly 65536 bytes");
        final String nul = OpenRequest.share("su\u0000b", "te\u0000xt", null, null);
        check(wellFormed(nul) && nul.equals("4\tsub\ttext\t0"), "NUL is taken out of the subject and the text: " + nul);
        check(OpenRequest.share("s", "x".repeat(50_000_000), null, null) != null, "a 50 MB text is cut fast");
        final String surrogate = OpenRequest.share("a\ud800b", "x", null, null);
        check(wellFormed(surrogate), "a lone surrogate still makes valid UTF-8");

        // The encoding and the cuts
        check(OpenRequest.encode("aZ09-._~ /?#%ü😀").equals("aZ09-._~%20%2F%3F%23%25%C3%BC%F0%9F%98%80"),
                "unreserved stay, every other byte is %XX");
        check(OpenRequest.truncate("a😀b", 4).equals("a"), "a character is not cut in the middle (4 bytes)");
        check(OpenRequest.truncate("a😀b", 5).equals("a😀"), "a 4-byte character fits exactly");
        check(OpenRequest.truncate("abc", 3).equals("abc") && OpenRequest.truncate("abc", 0).isEmpty(), "bounds");
        check(OpenRequest.mimeType("image/png") && OpenRequest.mimeType("application/vnd.a+b") && !OpenRequest.mimeType("image")
                && !OpenRequest.mimeType("a/b/c") && !OpenRequest.mimeType("a/") && !OpenRequest.mimeType("/b")
                && !OpenRequest.mimeType("a b/c") && !OpenRequest.mimeType("a/" + "b".repeat(100)) && !OpenRequest.mimeType("*/*"),
                "MIME types");

        if (failures > 0) {
            System.out.println("OpenRequestTest: " + failures + " failures");
            System.exit(1);
        }
        System.out.println("OpenRequestTest: fine");
    }
}
