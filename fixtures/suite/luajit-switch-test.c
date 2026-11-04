// luajit-switch-test: an app that starts a Lua state the way an engine does (the LuaJIT switch,
// runtime/android/luajit.c). Without an argument the engine's library is linked in and its names are imported;
// "dlsym-luajit" and "dlsym-lua" open libfake-luajit.so or libfake-lua.so and look the names up. Expected output:
// fixtures/expected/luajit-switch-test.out (luajit, as Android prints it) and the two other files beside it, with the
// line the switch adds in runtime/tests.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

void *luaL_newstate(void);
void luaL_openlibs(void *L);
const char *fake_lua_report(void *L);
const char *fake_lua_hot_loop(void *L);

int main(int argc, char **argv) {
    void *(*newstate)(void) = luaL_newstate;
    void (*openlibs)(void *) = luaL_openlibs;
    const char *(*report)(void *) = fake_lua_report;
    const char *(*hot_loop)(void *) = fake_lua_hot_loop;
    if (argc > 1) {
        void *library = dlopen(strcmp(argv[1], "dlsym-lua") == 0 ? "libfake-lua.so" : "libfake-luajit.so", RTLD_NOW);
        if (library == NULL) {
            printf("luajit-switch-test: %s\n", dlerror());
            return 1;
        }
        newstate = (void *(*)(void))dlsym(library, "luaL_newstate");
        openlibs = (void (*)(void *))dlsym(library, "luaL_openlibs");
        report = (const char *(*)(void *))dlsym(library, "fake_lua_report");
        hot_loop = (const char *(*)(void *))dlsym(library, "fake_lua_hot_loop");
        if (newstate == NULL || openlibs == NULL || report == NULL || hot_loop == NULL) {
            printf("luajit-switch-test: a name is missing\n");
            return 1;
        }
    }
    void *L = newstate();
    openlibs(L);
    printf("luajit-switch-test: %s\n", report(L));
    printf("luajit-switch-test: %s\n", hot_loop(L));
    return 0;
}
