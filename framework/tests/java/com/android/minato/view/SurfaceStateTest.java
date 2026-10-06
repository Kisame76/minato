package com.android.minato.view;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * ADR-0043 step c4 on the host's JVM (tools/framework-host-check.sh): our SurfaceView's state against a fake session.
 * The session's requests and the holder's callbacks go into one log, so their order is checked too: a surface is asked
 * for once the view may have one, its callbacks come with the session's answer, a move or a z order is a place without
 * callbacks, a new size is a place whose answer gives surfaceChanged, a hidden view, a stopped window or a detach give
 * surfaceDestroyed before the remove, a new format a new surface, and an answer for an older surface is dropped.
 */
public final class SurfaceStateTest {
    private static final int RGB_565 = 4;
    private static final int RGBA_8888 = 1;
    private static final int MEDIA = -2;
    private static final int ON_TOP = 1;

    private final List<String> mLog = new ArrayList<>();
    private int mAnswer;
    private final SurfaceState mState = new SurfaceState(new SurfaceState.Session() {
        @Override
        public int make(long token, int z, int left, int top, int right, int bottom, int width, int height,
                int format) {
            mLog.add("make " + name(token) + " z " + z + " " + left + "," + top + "," + right + "," + bottom + " size "
                    + width + "x" + height + " format " + format);
            return mAnswer;
        }

        @Override
        public int place(long token, int z, int left, int top, int right, int bottom, int width, int height) {
            mLog.add("place " + name(token) + " z " + z + " " + left + "," + top + "," + right + "," + bottom + " size "
                    + width + "x" + height);
            return 0;
        }

        @Override
        public int remove(long token) {
            mLog.add("remove " + name(token));
            return 0;
        }
    }, new SurfaceState.Callbacks() {
        @Override
        public void created(long window, int width, int height) {
            mLog.add("created window " + window + " " + width + "x" + height);
        }

        @Override
        public void changed(int format, int width, int height) {
            mLog.add("changed " + format + " " + width + "x" + height);
        }

        @Override
        public void redrawNeeded() {
            mLog.add("redraw");
        }

        @Override
        public void destroyed() {
            mLog.add("destroyed");
        }

        @Override
        public void unsupported(int error) {
            mLog.add("unsupported " + error);
        }
    });

    /** Tokens by the order the test met them, so that the expectations do not depend on other tests' tokens. */
    private final List<Long> mTokens = new ArrayList<>();

    private String name(long token) {
        if (!mTokens.contains(token)) {
            mTokens.add(token);
        }
        return "t" + (mTokens.indexOf(token) + 1);
    }

    private long token(int number) {
        return mTokens.get(number - 1);
    }

    /** A full-screen view on a visible window with its surface, as GLSurfaceView fills surface-test's. */
    private void shown() {
        mState.setFormat(RGB_565);
        mState.setAttached(true);
        mState.setViewVisible(true);
        mState.setWindowVisible(true);
        mState.setWindowStopped(false);
        mState.setWindowSurface(true);
        mState.setFrame(0, 0, 1290, 2796);
        mState.update();
    }

    private void expect(String... lines) {
        List<String> wanted = Arrays.asList(lines);
        if (!mLog.equals(wanted)) {
            throw new AssertionError("expected " + wanted + ", got " + mLog);
        }
        mLog.clear();
    }

    // Asking for a surface.

    public void testNothingUntilTheViewMayHaveASurface() {
        mState.setFormat(RGB_565);
        mState.setAttached(true);
        mState.setViewVisible(true);
        mState.setWindowVisible(true);
        mState.setWindowSurface(true);
        mState.update();
        expect();
        mState.setFrame(0, 0, 1290, 0);
        mState.update();
        expect();
        mState.setFrame(0, 0, 1290, 2796);
        mState.setWindowVisible(false);
        mState.update();
        expect();
        mState.setWindowVisible(true);
        mState.setWindowSurface(false);
        mState.update();
        expect();
        mState.setWindowSurface(true);
        mState.update();
        expect("make t1 z -2 0,0,1290,2796 size 0x0 format 4");
    }

    public void testOneMakeAndItsAnswerGivesTheThreeCallbacks() {
        shown();
        mState.update();
        mState.update();
        expect("make t1 z -2 0,0,1290,2796 size 0x0 format 4");
        mState.onCreated(token(1), 7, 1290, 2796);
        expect("created window 7 1290x2796", "changed 4 1290x2796", "redraw");
        check(mState.isCreated() && mState.window() == 7, "created on window 7");
        mState.update();
        expect();
    }

    // Changes of a made surface.

    public void testAMoveIsAPlaceWithoutCallbacks() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.setFrame(0, 100, 1290, 2896);
        mState.update();
        expect("place t1 z -2 0,100,1290,2896 size 0x0");
    }

    public void testAZOrderIsAPlace() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.setZ(ON_TOP);
        mState.update();
        expect("place t1 z 1 0,0,1290,2796 size 0x0");
    }

    public void testAFixedSizeIsAPlaceWhoseAnswerGivesChanged() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.setFixedSize(640, 480);
        mState.update();
        expect("place t1 z -2 0,0,1290,2796 size 640x480");
        mState.onResized(token(1), 640, 480);
        expect("changed 4 640x480", "redraw");
        check(mState.width() == 640 && mState.height() == 480, "the new size");
    }

    public void testANewFormatIsANewSurface() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.setFormat(RGBA_8888);
        mState.update();
        expect("destroyed", "remove t1", "make t2 z -2 0,0,1290,2796 size 0x0 format 1");
        mState.onCreated(token(2), 8, 1290, 2796);
        expect("created window 8 1290x2796", "changed 1 1290x2796", "redraw");
    }

    // Losing the surface.

    public void testAHiddenViewDestroysBeforeTheRemove() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.setViewVisible(false);
        mState.update();
        expect("destroyed", "remove t1");
        check(!mState.isCreated(), "not created");
        mState.setViewVisible(true);
        mState.update();
        expect("make t2 z -2 0,0,1290,2796 size 0x0 format 4");
    }

    public void testAStoppedWindowDestroysAndAStartMakesAgain() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.setWindowStopped(true);
        mState.update();
        expect("destroyed", "remove t1");
        mState.setWindowStopped(false);
        mState.update();
        expect("make t2 z -2 0,0,1290,2796 size 0x0 format 4");
    }

    public void testADetachDestroys() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.setAttached(false);
        mState.update();
        expect("destroyed", "remove t1");
    }

    public void testASurfaceRemovedBeforeItsAnswerGivesNoCallbacks() {
        shown();
        mState.setViewVisible(false);
        mState.update();
        expect("make t1 z -2 0,0,1290,2796 size 0x0 format 4", "remove t1");
        mState.onCreated(token(1), 7, 1290, 2796);
        expect();
    }

    public void testAnOlderSurfacesAnswersAreDropped() {
        shown();
        mState.setViewVisible(false);
        mState.update();
        mState.setViewVisible(true);
        mState.update();
        expect("make t1 z -2 0,0,1290,2796 size 0x0 format 4", "remove t1",
                "make t2 z -2 0,0,1290,2796 size 0x0 format 4");
        mState.onCreated(token(1), 7, 1290, 2796);
        mState.onResized(token(1), 640, 480);
        mState.onDestroyed(token(1));
        expect();
        mState.onCreated(token(2), 8, 1290, 2796);
        expect("created window 8 1290x2796", "changed 4 1290x2796", "redraw");
    }

    public void testTheHostTakesItAwayUntilTheWindowComesBack() {
        shown();
        mState.onCreated(token(1), 7, 1290, 2796);
        mLog.clear();
        mState.onDestroyed(token(1));
        expect("destroyed", "remove t1");
        mState.update();
        expect();
        mState.setWindowSurface(false);
        mState.update();
        mState.setWindowSurface(true);
        mState.update();
        expect("make t2 z -2 0,0,1290,2796 size 0x0 format 4");
    }

    public void testAHostWithoutSurfacesIsToldOnce() {
        mAnswer = -95;
        shown();
        expect("make t1 z -2 0,0,1290,2796 size 0x0 format 4", "unsupported -95");
        mState.setFrame(0, 10, 1290, 2806);
        mState.update();
        mState.setViewVisible(false);
        mState.update();
        mState.setViewVisible(true);
        mState.update();
        expect();
        check(!mState.isCreated(), "no surface");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : SurfaceStateTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new SurfaceStateTest());
            } catch (java.lang.reflect.InvocationTargetException thrown) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + thrown.getCause());
            }
        }
        if (failed > 0) {
            System.out.println("SurfaceStateTest: " + failed + " of " + run + " failed");
            System.exit(1);
        }
        System.out.println("SurfaceStateTest: " + run + " of " + run + " fine");
    }
}
