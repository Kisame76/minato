package com.android.net.module.util;

import android.text.TextUtils;

/**
 * Ours in place of the connectivity module's com.android.net.module.util.ProxyUtils (packages/modules/Connectivity's
 * staticlibs), which is not fetched: the one function android.net.Proxy calls, which joins a proxy's exclusion list
 * with commas.
 */
public class ProxyUtils {
    private ProxyUtils() {}

    /** The exclusion list as one comma separated string; an empty string for a null or empty list. */
    public static String exclusionListAsString(String[] exclusionList) {
        if (exclusionList == null) {
            return "";
        }
        return TextUtils.join(",", exclusionList);
    }
}
