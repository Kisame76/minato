#import "ensure_method.h"

#import <Foundation/Foundation.h>

BOOL minato_godot_ensure_method(id object, SEL selector, IMP implementation, const char *types) {
    if (object == nil || [object respondsToSelector:selector]) {
        return NO;
    }
    return class_addMethod(object_getClass(object), selector, implementation, types);
}

BOOL minato_godot_ensure_ivar(id object, const char *name, id (^make)(void)) {
    Ivar ivar = object == nil ? NULL : class_getInstanceVariable(object_getClass(object), name);
    const char *type = ivar == NULL ? NULL : ivar_getTypeEncoding(ivar);
    if (type == NULL || type[0] != '@' || object_getIvar(object, ivar) != nil) {
        return NO;
    }
    id value = make();
    if (value == nil) {
        return NO;
    }
    object_setIvarWithStrongDefault(object, ivar, value);
    return YES;
}
