package com.android.minato.permissions;

import java.lang.reflect.Method;
import java.util.List;

/**
 * What the host answered for the permission groups it decides (ADR-0063 decision 2) on the host's JVM
 * (tools/framework-host-check.sh): the stored answer checkSelfPermission reads, DENIED before the first; whether a
 * request must go to the host; Android's rationale after one denial and its "don't ask again" after the second; and
 * the answer of a foreground re-read, which updates the grant without counting as a denial.
 */
public final class PermissionAnswersTest {
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final int LOCATION = PermissionPolicy.GROUP_LOCATION;

    public void testNothingIsGrantedBeforeTheFirstAnswer() {
        PermissionAnswers answers = new PermissionAnswers();
        check(!answers.granted(FINE) && !answers.granted(COARSE), "denied before the first answer");
        check(answers.mustAsk(FINE) && answers.mustAsk(COARSE), "both can be asked");
        check(!answers.shouldShowRationale(FINE), "no rationale before the first ask");
    }

    public void testAGrantOfFullAccuracyGrantsBoth() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.GRANTED_FINE, true);
        check(answers.granted(FINE) && answers.granted(COARSE), "fine grants both");
        check(!answers.mustAsk(FINE) && !answers.mustAsk(COARSE), "nothing left to ask");
    }

    public void testAReducedGrantGrantsCoarseOnly() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.GRANTED_COARSE, true);
        check(answers.granted(COARSE) && !answers.granted(FINE), "coarse only");
        check(!answers.mustAsk(COARSE) && answers.mustAsk(FINE), "fine can be asked again, as Android's dialog allows");
    }

    public void testTheFirstDenialShowsARationaleAndTheSecondIsFinal() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.DENIED, true);
        check(!answers.granted(FINE), "denied");
        check(answers.shouldShowRationale(FINE) && answers.shouldShowRationale(COARSE), "rationale after one denial");
        check(answers.mustAsk(FINE), "asked again after one denial");
        answers.answer(LOCATION, PermissionAnswers.DENIED, true);
        check(!answers.shouldShowRationale(FINE), "no rationale after the second");
        check(!answers.mustAsk(FINE) && !answers.mustAsk(COARSE), "don't ask again: no dialog, denied at once");
    }

    public void testAGrantAfterADenialClearsIt() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.DENIED, true);
        answers.answer(LOCATION, PermissionAnswers.GRANTED_FINE, true);
        check(answers.granted(FINE) && !answers.shouldShowRationale(FINE), "granted, no rationale");
    }

    public void testAForegroundReadUpdatesWithoutCountingADenial() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.GRANTED_FINE, true);
        answers.answer(LOCATION, PermissionAnswers.DENIED, false);
        check(!answers.granted(FINE), "a revocation in iOS's Settings denies");
        check(!answers.shouldShowRationale(FINE) && answers.mustAsk(FINE), "and was no denial of an ask");
        answers.answer(LOCATION, PermissionAnswers.GRANTED_COARSE, false);
        check(answers.granted(COARSE) && !answers.granted(FINE), "a later grant, reduced");
    }

    public void testOtherPermissionsAndGroupsAreNotTheStoresBusiness() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(2, 1, true);
        check(!answers.granted("android.permission.CAMERA") && !answers.mustAsk("android.permission.CAMERA"),
                "a permission without a host group is never asked");
        check(!answers.granted(null) && !answers.mustAsk(null) && !answers.shouldShowRationale(null), "null");
        answers.answer(LOCATION, 7, true);
        check(!answers.granted(FINE), "an unknown outcome grants nothing");
    }

    public void testTheDetailOfARequest() {
        check(PermissionAnswers.locationDetail(List.of(FINE)) == 1, "fine");
        check(PermissionAnswers.locationDetail(List.of(FINE, COARSE)) == 1, "both ask for fine");
        check(PermissionAnswers.locationDetail(List.of(COARSE)) == 2, "coarse only");
        check(PermissionAnswers.locationDetail(List.of("android.permission.CAMERA")) == 0, "none");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : PermissionAnswersTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new PermissionAnswersTest());
            } catch (java.lang.reflect.InvocationTargetException e) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + e.getCause());
            }
        }
        System.out.println("PermissionAnswersTest: " + (run - failed) + " of " + run + " fine");
        if (failed != 0) {
            System.exit(1);
        }
    }
}
