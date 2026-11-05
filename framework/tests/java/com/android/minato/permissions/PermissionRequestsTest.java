package com.android.minato.permissions;

import java.lang.reflect.Method;
import java.util.List;
import java.util.function.Function;

/**
 * Which of an app's requested permissions go to the host and with what precision (ADR-0063 decision 2) on the
 * host's JVM (tools/framework-host-check.sh): only a permission that PermissionPolicy leaves to the host and that
 * Android would still ask about counts; a request that needs no dialog is answered at once.
 */
public final class PermissionRequestsTest {
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String INTERNET = "android.permission.INTERNET";
    private static final int LOCATION = PermissionPolicy.GROUP_LOCATION;

    private static final Function<String, PermissionPolicy.Grant> HOST_DECIDES = name ->
            PermissionPolicy.hostGroup(name) == LOCATION
                    ? PermissionPolicy.Grant.ASK_HOST : PermissionPolicy.Grant.GRANTED;

    public void testAPermissionThatNeedsNoDialogAsksNothing() {
        check(PermissionRequests.detailToAsk(List.of(INTERNET), HOST_DECIDES, new PermissionAnswers()) == 0, "normal");
        check(PermissionRequests.detailToAsk(List.of(), HOST_DECIDES, new PermissionAnswers()) == 0, "empty");
    }

    public void testFineAndCoarseTogetherAskForFine() {
        check(PermissionRequests.detailToAsk(List.of(COARSE, FINE), HOST_DECIDES, new PermissionAnswers()) == 1,
                "both");
        check(PermissionRequests.detailToAsk(List.of(FINE), HOST_DECIDES, new PermissionAnswers()) == 1, "fine alone");
    }

    public void testCoarseAloneAsksForCoarse() {
        check(PermissionRequests.detailToAsk(List.of(COARSE), HOST_DECIDES, new PermissionAnswers()) == 2, "coarse");
    }

    public void testAPermissionThePolicyDeniesIsNotAsked() {
        Function<String, PermissionPolicy.Grant> fineUndeclared = name ->
                FINE.equals(name) ? PermissionPolicy.Grant.DENIED_FIXED : HOST_DECIDES.apply(name);
        check(PermissionRequests.detailToAsk(List.of(FINE, COARSE), fineUndeclared, new PermissionAnswers()) == 2,
                "only coarse is declared");
        check(PermissionRequests.detailToAsk(List.of(FINE), fineUndeclared, new PermissionAnswers()) == 0,
                "nothing declared");
    }

    public void testWhatIsGrantedIsNotAskedAgain() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.GRANTED_FINE, true);
        check(PermissionRequests.detailToAsk(List.of(FINE, COARSE), HOST_DECIDES, answers) == 0, "granted");
    }

    public void testAReducedGrantLeavesFineToAsk() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.GRANTED_COARSE, true);
        check(PermissionRequests.detailToAsk(List.of(FINE, COARSE), HOST_DECIDES, answers) == 1, "fine again");
        check(PermissionRequests.detailToAsk(List.of(COARSE), HOST_DECIDES, answers) == 0, "coarse is held");
    }

    public void testTwoDenialsEndTheAsking() {
        PermissionAnswers answers = new PermissionAnswers();
        answers.answer(LOCATION, PermissionAnswers.DENIED, true);
        check(PermissionRequests.detailToAsk(List.of(FINE), HOST_DECIDES, answers) == 1, "after one denial");
        answers.answer(LOCATION, PermissionAnswers.DENIED, true);
        check(PermissionRequests.detailToAsk(List.of(FINE), HOST_DECIDES, answers) == 0, "after two, final");
    }

    public void testNotificationsAskLast() {
        java.util.function.Function<String, PermissionPolicy.Grant> host = name -> PermissionPolicy.Grant.ASK_HOST;
        java.util.List<int[]> groups = PermissionRequests.groupsToAsk(java.util.List.of(
                "android.permission.POST_NOTIFICATIONS", "android.permission.CAMERA"), host, new PermissionAnswers());
        check(groups.size() == 2 && groups.get(0)[0] == PermissionPolicy.GROUP_CAMERA
                && groups.get(1)[0] == PermissionPolicy.GROUP_NOTIFICATIONS && groups.get(1)[1] == 0,
                "camera, then notifications");
    }

    public void testTheCameraAsksAfterTheMicrophone() {
        PermissionAnswers answers = new PermissionAnswers();
        java.util.function.Function<String, PermissionPolicy.Grant> host = name -> PermissionPolicy.Grant.ASK_HOST;
        java.util.List<int[]> groups = PermissionRequests.groupsToAsk(java.util.List.of("android.permission.CAMERA",
                "android.permission.RECORD_AUDIO", "android.permission.ACCESS_FINE_LOCATION"), host, answers);
        check(groups.size() == 3 && groups.get(1)[0] == PermissionPolicy.GROUP_MICROPHONE
                && groups.get(2)[0] == PermissionPolicy.GROUP_CAMERA && groups.get(2)[1] == 0,
                "location, mic, then camera");
        answers.answer(PermissionPolicy.GROUP_CAMERA, PermissionAnswers.GRANTED_FINE, true);
        groups = PermissionRequests.groupsToAsk(java.util.List.of("android.permission.CAMERA"), host, answers);
        check(groups.isEmpty(), "a granted camera is not asked again");
    }

    public void testTheMicrophoneAsksAfterTheLocation() {
        PermissionAnswers answers = new PermissionAnswers();
        java.util.function.Function<String, PermissionPolicy.Grant> host = name -> PermissionPolicy.Grant.ASK_HOST;
        java.util.List<int[]> groups = PermissionRequests.groupsToAsk(
                java.util.List.of("android.permission.RECORD_AUDIO", "android.permission.ACCESS_FINE_LOCATION"), host,
                answers);
        check(groups.size() == 2 && groups.get(0)[0] == PermissionPolicy.GROUP_LOCATION && groups.get(0)[1] == 1
                && groups.get(1)[0] == PermissionPolicy.GROUP_MICROPHONE && groups.get(1)[1] == 0,
                "location, then mic");
        answers.answer(PermissionPolicy.GROUP_MICROPHONE, PermissionAnswers.GRANTED_FINE, true);
        groups = PermissionRequests.groupsToAsk(java.util.List.of("android.permission.RECORD_AUDIO"), host, answers);
        check(groups.isEmpty(), "a granted microphone is not asked again");
        groups = PermissionRequests.groupsToAsk(java.util.List.of("android.permission.RECORD_AUDIO"),
                name -> PermissionPolicy.Grant.DENIED_FIXED, new PermissionAnswers());
        check(groups.isEmpty(), "a denial for good is not asked");
    }

    public void testTheRequestTextIsGroupAndDetail() {
        final String text = PermissionRequests.requestText(LOCATION, 2);
        check("1\t2".equals(text), "text: " + text);
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : PermissionRequestsTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new PermissionRequestsTest());
            } catch (java.lang.reflect.InvocationTargetException e) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + e.getCause());
            }
        }
        System.out.println("PermissionRequestsTest: " + (run - failed) + " of " + run + " fine");
        if (failed != 0) {
            System.exit(1);
        }
    }
}
