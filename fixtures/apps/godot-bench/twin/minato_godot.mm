// The twin of test 2 (ADR-0042 decision 3): our wrapper around Godot 4.7.2's iOS template library (libgodot.a of
// the export templates' ios.zip), which tools/build-godot.sh --twin links into MinatoGodot.framework, against
// ADR-0030's MoltenVK 1.4.2 instead of the template's own. Only benchmark builds carry the framework, and the
// benchmark mode opens it with dlopen: it starts Godot with a run's own arguments, then shows Godot's view
// controller. Godot's app.swift, whose @main would clash with Minato's, stays out of the link, and every symbol
// but the two entries of exports.txt stays hidden.

#import <UIKit/UIKit.h>

#import "ensure_method.h"

// The template ships no headers. These mirror Godot 4.7.2-stable's drivers/apple_embedded/godot_view_controller.h
// and app_delegate_service.h.
@interface GDTViewController : UIViewController
@end

@interface GDTAppDelegateService : NSObject <UIApplicationDelegate, UIWindowSceneDelegate>
@property(strong, class, nonatomic) GDTViewController *viewController;
@end

// platform/ios/main_ios.mm, with C++ linkage: sets up the engine (Main::setup) for these arguments.
int apple_embedded_main(int argc, char **argv);

// platform/ios/api/api.h, with C++ linkage: Godot's exporter writes them into the app's dummy.cpp to register its
// plugins. The twin has none, and no camera module.
void godot_apple_embedded_plugins_initialize() {}
void godot_apple_embedded_plugins_deinitialize() {}

// The window that Godot's view asks the app delegate for on every frame, for the interface's orientation: the key
// window of the active scene, else of any. Godot's own app delegate has one; Minato's has none.
static UIWindow *minato_godot_window(id self, SEL command) {
    (void)self;
    (void)command;
    UIWindowScene *any = nil;
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) {
            continue;
        }
        UIWindowScene *windowScene = (UIWindowScene *)scene;
        if (scene.activationState == UISceneActivationStateForegroundActive) {
            return windowScene.keyWindow ?: windowScene.windows.firstObject;
        }
        any = any ?: windowScene;
    }
    return any.keyWindow ?: any.windows.firstObject;
}

// Godot's delegate service, which turns the scenes' events into the engine's focus and background events. In
// Godot's app its delegate passes them on; in the twin the notifications do. It lives as long as the process.
static GDTAppDelegateService *service;

static void minato_godot_forward_scene_events(void) {
    service = [[GDTAppDelegateService alloc] init];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:UISceneWillDeactivateNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        [service sceneWillResignActive:note.object];
    }];
    [center addObserverForName:UISceneDidActivateNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        [service sceneDidBecomeActive:note.object];
    }];
    [center addObserverForName:UISceneDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        [service sceneDidEnterBackground:note.object];
    }];
    [center addObserverForName:UISceneWillEnterForegroundNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        [service sceneWillEnterForeground:note.object];
    }];
    [center addObserverForName:UISceneDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification *note) {
        [service sceneDidDisconnect:note.object];
    }];
}

// Sets up the engine on the main thread, before the view controller's view first draws: Godot's renderer ends the
// process when it draws without an engine. argv[0] must hold a '/', as an executable's path does. Returns 0, or the
// error of Main::setup.
// It calls apple_embedded_main itself, as Godot's didFinishLaunching does, without the audio session that one sets
// (the bench plays no sound). Godot starts unfocused and the app's scene is active already, so the engine gets
// one focus-in here; later scene events follow through the notifications.
extern "C" __attribute__((visibility("default"))) int minato_godot_start(int argc, char **argv) {
    minato_godot_ensure_method(UIApplication.sharedApplication.delegate, @selector(window), (IMP)minato_godot_window, "@@:");
    int error = apple_embedded_main(argc, argv);
    if (error != 0) {
        return error;
    }
    minato_godot_forward_scene_events();
    [service sceneDidBecomeActive:UIApplication.sharedApplication.connectedScenes.anyObject];
    return 0;
}

// Godot's view controller, retained for the caller (CFBridgingRelease), and the one that Godot's display server
// finds its layer through. Godot keeps one engine per process: call it once.
extern "C" __attribute__((visibility("default"))) void *minato_godot_view_controller(void) {
    GDTViewController *viewController = [[GDTViewController alloc] init];
    GDTAppDelegateService.viewController = viewController;
    return (__bridge_retained void *)viewController;
}
