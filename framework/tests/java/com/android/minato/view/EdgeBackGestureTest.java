package com.android.minato.view;

import java.lang.reflect.Method;

/**
 * ADR-0041 decision 10 step b2's edge back gesture on the host's JVM (tools/framework-host-check.sh): which touches
 * become Android 15's back, as measured on apk-runner-35 in gesture navigation with edge-back-test (430 x 932 dp at
 * density 3): a down from 29 dp of the left or right edge starts one, from 30 dp not; a swipe of 18 dp starts it, one
 * of 16 dp stays the app's; an up 18 dp from the down cancels it, one at 20 dp commits it, and so does one after a
 * stop, but not one back at the edge; a swipe mostly down stays the app's; a down in an exclusion rect stays the
 * app's; the app gets its down and the first moves, then a cancel when the back starts, and nothing more of that
 * touch. With the system bars hidden to be shown by a swipe, the first edge swipe is the app's and shows them for a
 * while, in which the next one is a back.
 */
public final class EdgeBackGestureTest {
    private static final float DENSITY = 3f;
    private static final float WIDTH = 430 * DENSITY, HEIGHT = 932 * DENSITY;
    private final EdgeBackGesture mGesture = new EdgeBackGesture(DENSITY);

    public EdgeBackGestureTest() {
        mGesture.setWindow(WIDTH, HEIGHT, new int[0]);
    }

    /** A swipe of `length` dp from `x` dp at `y` dp in 10 moves, then its up: the steps the gesture gave. */
    private String swipe(float x, float y, float length, boolean toTheLeft) {
        return swipe(0, x, y, length, toTheLeft);
    }

    /** The same to the right, whose down is at `time` ms of the uptime clock. */
    private String swipeAt(long time, float x, float y, float length) {
        return swipe(time, x, y, length, false);
    }

    private String swipe(long time, float x, float y, float length, boolean toTheLeft) {
        mGesture.setTime(time);
        StringBuilder steps = new StringBuilder(name(mGesture.down(x * DENSITY, y * DENSITY)));
        float direction = toTheLeft ? -1 : 1;
        int last = EdgeBackGesture.APP;
        for (int i = 1; i <= 10; i++) {
            int step = mGesture.move((x + direction * length * i / 10) * DENSITY, y * DENSITY);
            if (step != last) {
                steps.append(' ').append(name(step));
                last = step;
            }
        }
        steps.append(' ').append(name(mGesture.up((x + direction * length) * DENSITY, y * DENSITY)));
        return steps.toString();
    }

    public void testASwipeFromTheLeftEdgeIsTheBack() {
        checkEquals("app start progress commit", swipe(2, 652, 250, false), "from 2 dp");
        checkEquals("app start progress commit", swipe(29, 652, 250, false), "from 29 dp");
    }

    public void testASwipeFromFurtherInIsTheApps() {
        checkEquals("app app", swipe(30, 652, 250, false), "from 30 dp");
        checkEquals("app app", swipe(44, 652, 250, false), "from 44 dp");
    }

    public void testTheRightEdgeIsTheSame() {
        checkEquals("app start progress commit", swipe(428, 652, 250, true), "from 2 dp of the right edge");
        checkEquals("app start progress commit", swipe(401, 652, 250, true), "from 29 dp of the right edge");
        checkEquals("app app", swipe(400, 652, 250, true), "from 30 dp of the right edge");
    }

    public void testAShortSwipeIsTheApps() {
        checkEquals("app app", swipe(2, 652, 10, false), "10 dp");
        checkEquals("app app", swipe(2, 652, 16, false), "16 dp");
    }

    public void testAnUpCloseToTheDownCancels() {
        checkEquals("app start cancel", swipe(2, 652, 18, false), "18 dp");
        checkEquals("app start progress commit", swipe(2, 652, 20, false), "20 dp");
    }

    public void testAnUpBackAtTheEdgeCancelsAndOneAfterAStopCommits() {
        mGesture.down(2 * DENSITY, 652 * DENSITY);
        checkEquals("start", name(mGesture.move(60 * DENSITY, 652 * DENSITY)), "out");
        checkEquals("progress", name(mGesture.move(2 * DENSITY, 652 * DENSITY)), "back");
        checkEquals("cancel", name(mGesture.up(2 * DENSITY, 652 * DENSITY)), "the up at the edge");
        mGesture.down(2 * DENSITY, 652 * DENSITY);
        mGesture.move(150 * DENSITY, 652 * DENSITY);
        checkEquals("commit", name(mGesture.up(150 * DENSITY, 652 * DENSITY)), "the up where it stopped");
    }

    public void testHiddenBarsTakeTheFirstSwipeThenTheNextIsTheBack() {
        mGesture.setBarsHidden(true);
        long now = 1000;
        checkEquals("app app", swipeAt(now, 2, 652, 250), "the first swipe shows the bars");
        checkEquals("app start progress commit", swipeAt(now + 2000, 2, 652, 250), "the next one 2 s later");
        checkEquals("app app", swipeAt(now + 4000, 2, 652, 250), "after the back, the bars are hidden again");
        checkEquals("app app", swipeAt(now + 10000, 2, 652, 250), "this one shows them again");
        checkEquals("app app", swipeAt(now + 12500, 2, 652, 250), "2.5 s later they have gone again");
        mGesture.setBarsHidden(false);
        checkEquals("app start progress commit", swipeAt(now + 20000, 2, 652, 250), "with the bars shown");
    }

    public void testASwipeMostlyDownIsTheApps() {
        StringBuilder steps = new StringBuilder(name(mGesture.down(2 * DENSITY, 516 * DENSITY)));
        for (int i = 1; i <= 10; i++) {
            steps.append(' ').append(name(mGesture.move((2 + 3.8f * i) * DENSITY, (516 + 31.6f * i) * DENSITY)));
        }
        steps.append(' ').append(name(mGesture.up(40 * DENSITY, 832 * DENSITY)));
        checkEquals("app app app app app app app app app app app app", steps.toString(), "38 dp across, 316 down");
    }

    public void testAnExclusionRectKeepsTheEdge() {
        int top = Math.round(HEIGHT * 4 / 10), bottom = Math.round(HEIGHT * 5 / 10);
        mGesture.setWindow(WIDTH, HEIGHT, new int[] {0, top, Math.round(48 * DENSITY), bottom});
        checkEquals("app app", swipe(2, 419, 250, false), "inside the band");
        checkEquals("app start progress commit", swipe(2, 652, 250, false), "below the band");
    }

    public void testASecondFingerEndsIt() {
        mGesture.down(2 * DENSITY, 652 * DENSITY);
        checkEquals("app", name(mGesture.pointerDown()), "a second finger before the start");
        checkEquals("app", name(mGesture.move(100 * DENSITY, 652 * DENSITY)), "the move after it");
        mGesture.up(100 * DENSITY, 652 * DENSITY);
        mGesture.down(2 * DENSITY, 652 * DENSITY);
        mGesture.move(30 * DENSITY, 652 * DENSITY);
        checkEquals("cancel", name(mGesture.pointerDown()), "a second finger after the start");
        checkEquals("swallow", name(mGesture.move(100 * DENSITY, 652 * DENSITY)), "the move after it");
        checkEquals("swallow", name(mGesture.up(100 * DENSITY, 652 * DENSITY)), "the up after it");
    }

    public void testTheNextTouchStartsAfresh() {
        swipe(2, 652, 250, false);
        checkEquals("app app", swipe(200, 652, 100, false), "a swipe in the middle after a back");
    }

    private static String name(int step) {
        switch (step) {
            case EdgeBackGesture.APP: return "app";
            case EdgeBackGesture.START: return "start";
            case EdgeBackGesture.PROGRESS: return "progress";
            case EdgeBackGesture.COMMIT: return "commit";
            case EdgeBackGesture.CANCEL: return "cancel";
            case EdgeBackGesture.SWALLOW: return "swallow";
            default: return "step " + step;
        }
    }

    private static void checkEquals(String expected, String actual, String what) {
        if (!expected.equals(actual)) {
            throw new AssertionError(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : EdgeBackGestureTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new EdgeBackGestureTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("EdgeBackGestureTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("EdgeBackGestureTest: " + run + " of " + run + " fine");
    }
}
