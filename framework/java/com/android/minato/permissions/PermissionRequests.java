package com.android.minato.permissions;

import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.function.Function;

/**
 * What Activity.requestPermissions sends to the host (ADR-0063 decision 2): of the requested permissions only those
 * that PermissionPolicy leaves to the host and that Android would still ask about, with the precision the app wants.
 * Anything else needs no dialog and is answered at once. Plain Java, so tools/framework-host-check.sh runs it on the
 * host's JVM.
 */
public final class PermissionRequests {
    private PermissionRequests() {
    }

    /**
     * The detail of APKRT_REQUEST_PERMISSION's location text for the permissions that need the host: 1 when fine
     * location is among them, 2 for coarse only, and 0 when no dialog is needed.
     *
     * @param names   the permissions the app requests
     * @param grantOf PermissionPolicy's grant for a permission of the app
     * @param answers what the host answered so far
     */
    public static int detailToAsk(Collection<String> names, Function<String, PermissionPolicy.Grant> grantOf,
            PermissionAnswers answers) {
        final List<String> asked = new ArrayList<>();
        for (String name : names) {
            if (grantOf.apply(name) == PermissionPolicy.Grant.ASK_HOST && answers.mustAsk(name)) {
                asked.add(name);
            }
        }
        return PermissionAnswers.locationDetail(asked);
    }

    /**
     * The groups that need the host, in the order Android's dialogs come, as {group, detail}: the location group
     * first, then the microphone; none when no dialog is needed.
     */
    public static List<int[]> groupsToAsk(Collection<String> names, Function<String, PermissionPolicy.Grant> grantOf,
            PermissionAnswers answers) {
        final List<int[]> groups = new ArrayList<>();
        final int location = detailToAsk(names, grantOf, answers);
        if (location != 0) {
            groups.add(new int[] {PermissionPolicy.GROUP_LOCATION, location});
        }
        for (String name : names) {
            if (PermissionPolicy.hostGroup(name) == PermissionPolicy.GROUP_MICROPHONE
                    && grantOf.apply(name) == PermissionPolicy.Grant.ASK_HOST && answers.mustAsk(name)) {
                groups.add(new int[] {PermissionPolicy.GROUP_MICROPHONE, 0});
                break;
            }
        }
        return groups;
    }

    /** APKRT_REQUEST_PERMISSION's text: the group and its detail, tab separated. */
    public static String requestText(int group, int detail) {
        return group + "\t" + detail;
    }
}
