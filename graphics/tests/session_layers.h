/* graphics-run's session layers (session_layers.m, ADR-0043 c4 (c) and ADR-0041 gap 4 (c)): what window_host.m and
   session_layers_test.m share with them. Objective-C. */
#ifndef MINATO_SESSION_LAYERS_H
#define MINATO_SESSION_LAYERS_H

#import <QuartzCore/QuartzCore.h>

/* MINATO_WINDOW_WATCH=hidden: prints each change of `layer`'s hidden to stderr, as "window host: the layer is hidden"
   or "shown"; NO without the variable. One layer at a time: the last one asked for. */
BOOL minato_test_watch_hidden(CALayer *layer);

/* For session_layers_test.m: the session's root layer, nil before the session window's first layer, and a wait until
   the tree's queue has made every change asked for so far. */
CALayer *minato_test_session_root(void);
void minato_test_session_settle(void);

#endif
