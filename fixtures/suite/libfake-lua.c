// libfake-lua.so: plain Lua by its names only: luaL_newstate and luaL_openlibs, no luaJIT_setmode (the LuaJIT switch
// leaves such a library alone).
#include <stdio.h>
#include <stdlib.h>

void *luaL_newstate(void) {
    printf("luajit-switch-test: luaL_newstate\n");
    return calloc(1, sizeof(int));
}

void luaL_openlibs(void *L) {
    (void)L;
    printf("luajit-switch-test: luaL_openlibs\n");
}

const char *fake_lua_report(void *L) {
    (void)L;
    return "plain Lua";
}

const char *fake_lua_hot_loop(void *L) {
    (void)L;
    return "no trace compiler";
}
