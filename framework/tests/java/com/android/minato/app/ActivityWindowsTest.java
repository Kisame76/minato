package com.android.minato.app;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * ADR-0041 gap 4 (b3) on the host's JVM (tools/framework-host-check.sh): which window each of the task's activities
 * has, as the plan's decision 3 and Android 15's ensureActivitiesVisible give it. From the top down every activity
 * that is not finishing is visible until the first that occludes the ones below it; the lowest visible one has the
 * session's window, unless it floats or has a requested window already, and every other visible one a requested
 * window; the rest have none. Each visible activity's key, the group of its windows, grows from the bottom up.
 */
public final class ActivityWindowsTest {
    private final ActivityWindows mWindows = new ActivityWindows();

    private static ActivityWindows.Slot opaque() {
        return new ActivityWindows.Slot();
    }

    private static ActivityWindows.Slot translucent() {
        ActivityWindows.Slot slot = new ActivityWindows.Slot();
        slot.translucentTheme = true;
        return slot;
    }

    private static ActivityWindows.Slot floating() {
        ActivityWindows.Slot slot = new ActivityWindows.Slot();
        slot.floatingTheme = true;
        return slot;
    }

    private List<ActivityWindows.Slot> update(ActivityWindows.Slot... bottomUp) {
        return mWindows.update(Arrays.asList(bottomUp));
    }

    private static String modes(ActivityWindows.Slot... slots) {
        List<String> modes = new ArrayList<>();
        for (ActivityWindows.Slot slot : slots) {
            modes.add(slot.mode() == ActivityWindows.SESSION ? "session"
                    : slot.mode() == ActivityWindows.REQUESTED ? "requested" : "hidden");
        }
        return String.join(" ", modes);
    }

    private static void increasing(ActivityWindows.Slot... visibleBottomUp) {
        int last = 0;
        for (ActivityWindows.Slot slot : visibleBottomUp) {
            check(slot.key() > last, "keys grow from the bottom up: " + slot.key() + " after " + last);
            last = slot.key();
        }
    }

    void testOneActivityHasTheSessionsWindow() {
        ActivityWindows.Slot a = opaque();
        check(update(a).equals(List.of(a)), "a new activity changed");
        check(modes(a).equals("session"), modes(a));
        check(mWindows.session() == a && mWindows.input() == a, "it has the session's window and input");
        check(a.key() > 0, "it has a key for its dialogs");
    }

    void testATranslucentTopGetsARequestedWindowAboveTheSessionsOne() {
        ActivityWindows.Slot a = opaque(), b = translucent();
        update(a);
        check(update(a, b).equals(List.of(b)), "only the new one changed");
        check(modes(a, b).equals("session requested"), modes(a, b));
        check(mWindows.session() == a && mWindows.input() == a, "the lowest visible keeps the session's window");
        increasing(a, b);
    }

    void testAnOpaqueTopHidesTheOnesBelow() {
        ActivityWindows.Slot a = opaque(), b = translucent(), c = opaque();
        update(a, b);
        check(update(a, b, c).equals(List.of(a, b, c)), "all three changed");
        check(modes(a, b, c).equals("hidden hidden session"), modes(a, b, c));
        check(mWindows.session() == c, "the opaque top has the session's window");
    }

    void testTheOnesBelowComeBackWhenTheTopFinishes() {
        ActivityWindows.Slot a = opaque(), b = translucent(), c = opaque();
        update(a, b);
        int keyA = a.key(), keyB = b.key();
        update(a, b, c);
        c.finishing = true;
        check(update(a, b, c).equals(List.of(a, b, c)), "all three changed");
        check(modes(a, b, c).equals("session requested hidden"), modes(a, b, c));
        check(a.key() == keyA && b.key() == keyB, "keys that still grow stay");
    }

    void testAFinishingTopIsSkipped() {
        ActivityWindows.Slot a = opaque(), b = translucent();
        b.finishing = true;
        update(a, b);
        check(modes(a, b).equals("session hidden"), modes(a, b));
    }

    void testAFloatingActivityAloneHasNoSessionsWindow() {
        ActivityWindows.Slot f = floating();
        update(f);
        check(modes(f).equals("requested"), modes(f));
        check(mWindows.session() == null, "the session's window stays empty below a floating activity");
        check(mWindows.input() == f, "the session's input reaches it");
    }

    void testAFloatingTopLetsTheOneBelowShow() {
        ActivityWindows.Slot a = opaque(), f = floating();
        update(a, f);
        check(modes(a, f).equals("session requested"), modes(a, f));
        increasing(a, f);
    }

    void testSetTranslucentChangesWhatTheTopOccludes() {
        ActivityWindows.Slot a = opaque(), b = translucent();
        update(a, b);
        b.converted = false;
        check(update(a, b).equals(List.of(a)), "only the one below changed");
        check(modes(a, b).equals("hidden requested"), modes(a, b));
        check(mWindows.session() == null && mWindows.input() == b, "the top keeps its window, as Android keeps it");
        b.converted = true;
        update(a, b);
        check(modes(a, b).equals("session requested"), modes(a, b));
        increasing(a, b);
    }

    void testTheLowestKeepsItsRequestedWindowWhileItStaysVisible() {
        ActivityWindows.Slot a = opaque(), b = translucent();
        update(a, b);
        a.finishing = true;
        check(update(a, b).equals(List.of(a)), "only the finishing one changed");
        check(modes(a, b).equals("hidden requested"), modes(a, b));
        check(mWindows.session() == null && mWindows.input() == b, "the empty session's window lies below it");
        ActivityWindows.Slot c = opaque();
        update(a, b, c);
        b.finishing = false;
        c.finishing = true;
        update(a, b, c);
        check(modes(a, b, c).equals("hidden session hidden"), "shown again, it has the session's window: "
                + modes(a, b, c));
    }

    void testAnOpaqueActivityConvertedToTranslucentShowsTheOneBelow() {
        ActivityWindows.Slot a = opaque(), b = opaque();
        update(a, b);
        check(modes(a, b).equals("hidden session"), modes(a, b));
        b.converted = true;
        update(a, b);
        check(modes(a, b).equals("session requested"), modes(a, b));
    }

    void testAFloatingActivityConvertedToOpaqueHidesTheOneBelowAndStillFloats() {
        ActivityWindows.Slot a = opaque(), f = floating();
        update(a, f);
        f.converted = false;
        update(a, f);
        check(modes(a, f).equals("hidden requested"), modes(a, f));
        check(mWindows.session() == null && mWindows.input() == f, "the floating one has the input, no window below");
    }

    void testAReorderedActivityGetsANewKey() {
        ActivityWindows.Slot a = opaque(), t = translucent(), u = translucent();
        update(a, t, u);
        int keyU = u.key();
        check(update(a, u, t).equals(List.of(t)), "only the one whose key no longer grows changed");
        check(u.key() == keyU, "the one moved down keeps its key");
        increasing(a, u, t);
    }

    void testNothingVisibleWhileAllFinish() {
        ActivityWindows.Slot a = opaque();
        update(a);
        a.finishing = true;
        check(update(a).equals(List.of(a)), "it changed");
        check(modes(a).equals("hidden"), modes(a));
        check(mWindows.session() == null && mWindows.input() == null, "no window and no input");
    }

    void testAPassWithoutChangesChangesNothing() {
        ActivityWindows.Slot a = opaque(), b = translucent();
        update(a, b);
        check(update(a, b).isEmpty(), "nothing changed");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : ActivityWindowsTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new ActivityWindowsTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("ActivityWindowsTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("ActivityWindowsTest: " + run + " of " + run + " fine");
    }
}
