// The twin's window fallback (ADR-0042 c) 2), apart from UIKit so that the Mac can check it (ensure_method_test.m).
#import <objc/runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

// Gives the object's class the method of the selector when the object does not respond to it, and returns YES then.
// An object that has the method, its own or inherited, keeps it.
BOOL minato_godot_ensure_method(id object, SEL selector, IMP implementation, const char *types);

#ifdef __cplusplus
}
#endif
