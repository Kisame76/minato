// The SIGSEGV handlers of fault-site-test's libraries: an engine's (libfault-site.c) and a crash reporter's that a second
// library installs over it (libfault-site-reporter.c), as an app's own crash handlers take a fault before the runtime's
// default action does.
#pragma once

/// How the engine's handler ends after it printed its line.
enum fault_site_ending {
    FAULT_SITE_RETURN = 1,   // restores the action it replaced and returns: the load faults again
    FAULT_SITE_RAISE = 2,    // restores it, raises SIGSEGV (blocked in the handler, so it waits) and returns
    FAULT_SITE_ABORT = 3,    // calls abort()
    FAULT_SITE_RECOVER = 4,  // moves pc past the load and returns: the program goes on
    FAULT_SITE_CRASH = 5,    // reads through a bad pointer itself, with SIGSEGV blocked
};

/// Installs the engine's handler on an alternate stack of its own, saving the action it replaces; 0 or -1.
int fault_site_install(int ending);

/// Installs the reporter's handler over whatever handles SIGSEGV now; it prints its line and calls the handler it
/// replaced, then returns. 0 or -1.
int fault_site_install_reporter(void);
