// What the parts of libandroid.so share and do not export (its version script keeps them local).
#ifndef MINATO_ANDROID_INTERNAL_H
#define MINATO_ANDROID_INTERNAL_H

#include <android/configuration.h>

#include "APKRuntime/guest_interface.h"

/// Sets every field of `config` to the value ADR-0028's decision 9 chooses from the session's configuration.
void minato_configuration_from_session(AConfiguration *config, const struct apkrt_configuration *session);

#endif
