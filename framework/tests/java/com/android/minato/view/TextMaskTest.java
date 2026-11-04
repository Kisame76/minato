package com.android.minato.view;

import java.lang.reflect.Method;

/**
 * Which fields' text the bar above the keyboard shows as bullets (ADR-0068, keyboard plan b), on the host's JVM
 * (tools/framework-host-check.sh): a password, a web password and a number password, as Android's keyboards hide
 * them; a visible password, which SDL2 asks for only to turn suggestions off, shows its text, as on Android.
 */
public final class TextMaskTest {
    private static final int TEXT = 0x1, NUMBER = 0x2, PHONE = 0x3;

    public void testAPasswordIsMasked() {
        check(TextMask.masks(TEXT | 0x80), "a text password");
        check(TextMask.masks(TEXT | 0xe0), "a web password");
        check(TextMask.masks(NUMBER | 0x10), "a number password");
        check(TextMask.masks(TEXT | 0x80 | 0x20000), "a multi-line password");
    }

    public void testAVisiblePasswordShowsItsText() {
        check(!TextMask.masks(TEXT | 0x90), "SDL2's visible password");
        check(!TextMask.masks(TEXT | 0x90 | 0x80000), "a visible password without suggestions");
    }

    public void testOtherFieldsShowTheirText() {
        check(!TextMask.masks(0), "no input type");
        check(!TextMask.masks(TEXT), "plain text");
        check(!TextMask.masks(TEXT | 0x20), "an email address");
        check(!TextMask.masks(NUMBER), "a number");
        check(!TextMask.masks(PHONE | 0x10), "a phone number's bits that a number password has");
        check(!TextMask.masks(NUMBER | 0x80), "a number with a text password's variation");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : TextMaskTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new TextMaskTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("TextMaskTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("TextMaskTest: " + run + " of " + run + " fine");
    }
}
