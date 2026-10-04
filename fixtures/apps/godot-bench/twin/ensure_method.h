// The twin's fallbacks (ADR-0042 c): the window (c) 2) and the GLES layer's context (c4.1), apart from UIKit and
// OpenGL ES so that the Mac can check them (ensure_method_test.m).
#import <objc/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

// Gives the object's class the method of the selector when the object does not respond to it, and returns YES then.
// An object that has the method, its own or inherited, keeps it.
BOOL minato_godot_ensure_method(id object, SEL selector, IMP implementation, const char *types);

// Sets the object's instance variable of that name, an object that is nil, to what make returns, held strongly, and
// returns YES then. An object without that variable, with an object in it or with a variable of another type keeps
// what it has, and so does one for which make returns nil; make runs only when the variable is set.
BOOL minato_godot_ensure_ivar(id object, const char *name, id (^make)(void));

#ifdef __cplusplus
}
#endif
