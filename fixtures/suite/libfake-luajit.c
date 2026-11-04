// libfake-luajit.so: stands in for libluajit by its exported names only (the LuaJIT switch, see
// runtime/android/luajit.c): luaL_newstate, luaL_openlibs, which turns the JIT engine on as luaopen_jit does, and
// luaJIT_setmode, which turns it off for the mode LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF (0). It prints the arguments of
// luaJIT_setmode and reports the engine through fake_lua_report. No Lua code: a state is a small struct.
#include <stdio.h>
#include <stdlib.h>

typedef struct { int jit_on; } State;

void *luaL_newstate(void) {
    printf("luajit-switch-test: luaL_newstate\n");
    return calloc(1, sizeof(State));
}

void luaL_openlibs(void *L) {
    printf("luajit-switch-test: luaL_openlibs\n");
    ((State *)L)->jit_on = 1;  // luaopen_jit turns the engine on
}

int luaJIT_setmode(void *L, int idx, int mode) {
    printf("luajit-switch-test: luaJIT_setmode(L, %d, %#x)\n", idx, mode);
    if (idx == 0 && (mode & 0xff) == 0 && (mode & 0x0f00) == 0) ((State *)L)->jit_on = 0;  // ENGINE | OFF
    return 1;
}

const char *fake_lua_report(void *L) {
    return ((State *)L)->jit_on ? "JIT engine on" : "JIT engine off";
}

// A hot loop: with the engine on, the trace compiler would reserve its machine code area and ask for it to be
// executable (lj_mcode_reserve, from lj_asm_trace on the first trace; LuaJIT's jit_init and luaopen_jit reserve
// nothing, read in lib_jit.c and lj_trace.c on 2026-11-04). The fake only says whether it would have.
const char *fake_lua_hot_loop(void *L) {
    return ((State *)L)->jit_on ? "the trace compiler asked for executable memory"
                                : "the trace compiler asked for nothing";
}
