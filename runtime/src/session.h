// The session (ADR-0028, decisions 2 to 5): what the host tells the runtime about the app before the
// program starts, once per process (apkrt_session_begin): the end handler, the configuration and the
// handler for the guest's requests; and the channel between them, the event pipe with its answers.
#pragma once

#include "APKRuntime/apkrt.h"

#include <cstdint>

namespace apkrt::session {

/// The session's end handler with its context; a null handler while no session has begun.
struct EndHandler {
    apkrt_end_handler function;
    void *context;
};
EndHandler end_handler();

/// At the program's end, before the end handler: posts fail from now on, and the host's waits for answers
/// end with -1.
void end();

/// The display's refresh rate in the session's configuration, frames per second; 0 without a session.
int refresh_rate();

/// 1 from the activity's onPause (APKRT_EVENT_PAUSE/STOP) until its onResume (APKRT_EVENT_RESUME), 0 otherwise
/// and before any session: the state libc.so's minato_activity_paused reports for the SDL swap override. The
/// address is stable for the process; the loader points libc's minato_activity_paused_flag at it (dl.cpp
/// preload_sdl_swap).
int *activity_paused_address();

/// The private syscalls (guest_interface.h): session_connect with the guest's struct apkrt_configuration,
/// session_configuration, session_answer and session_request.
long connect(std::uint64_t configuration);
long configuration(std::uint64_t address);
long answer(std::uint64_t serial);
long request(std::uint64_t type, std::uint64_t a0, std::uint64_t a1);

}  // namespace apkrt::session
