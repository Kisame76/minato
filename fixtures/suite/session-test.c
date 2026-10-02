// session-test (ADR-0039, config-test): the session's configuration as a program sees it, through the private
// syscall session_configuration and through libandroid's minato_AConfiguration_fromSession, which
// libandroid_runtime uses for Java's Configuration and DisplayMetrics. runtime.session_run starts it through apk-run
// with --configuration and without, where there is no session (runtime/tests/session_run.py).
#include <android/configuration.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

int main(void) {
    struct apkrt_configuration session;
    if (syscall(APKRT_NR_SESSION_CONFIGURATION, &session) != 0) {
        printf("session_configuration: error %d\n", errno);
    } else {
        printf("session_configuration: screen %d x %d px at scale %.1f, orientation %d, night %d, refresh %d, "
               "locale %s-%s\n",
               session.width, session.height, session.scale, session.orientation, session.night,
               session.refresh_rate, session.language, session.country);
    }

    void *android = dlopen("libandroid.so", RTLD_NOW);
    int (*from_session)(AConfiguration *, struct apkrt_configuration *) =
        android != NULL ? (int (*)(AConfiguration *, struct apkrt_configuration *))dlsym(
                              android, "minato_AConfiguration_fromSession")
                        : NULL;
    if (from_session == NULL) {
        printf("no minato_AConfiguration_fromSession\n");
        return 0;
    }
    AConfiguration *config = AConfiguration_new();
    const int result = from_session(config, &session);
    if (result != 0) {
        printf("AConfiguration: error %d\n", result);
    } else {
        char language[2], country[2];
        AConfiguration_getLanguage(config, language);
        AConfiguration_getCountry(config, country);
        printf("AConfiguration: density %d, screen %d x %d dp, smallest %d dp, orientation %d, locale %c%c-%c%c\n",
               AConfiguration_getDensity(config), AConfiguration_getScreenWidthDp(config),
               AConfiguration_getScreenHeightDp(config), AConfiguration_getSmallestScreenWidthDp(config),
               AConfiguration_getOrientation(config), language[0], language[1], country[0], country[1]);
    }
    AConfiguration_delete(config);
    return 0;
}
