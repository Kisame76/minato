#import "ensure_method.h"

#import <Foundation/Foundation.h>

BOOL minato_godot_ensure_method(id object, SEL selector, IMP implementation, const char *types) {
    if (object == nil || [object respondsToSelector:selector]) {
        return NO;
    }
    return class_addMethod(object_getClass(object), selector, implementation, types);
}
