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
