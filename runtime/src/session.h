// The session (ADR-0028, decision 3): what the host tells the runtime about the app before the program
// starts, once per process (apkrt_session_begin). So far its end handler (decision 5); the
// configuration, the events and the requests come with ADR-0028's step c.
#pragma once

#include "APKRuntime/apkrt.h"

namespace apkrt::session {

/// The session's end handler with its context; a null handler while no session has begun.
struct EndHandler {
    apkrt_end_handler function;
    void *context;
};
EndHandler end_handler();

}  // namespace apkrt::session
