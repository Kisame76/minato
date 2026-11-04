// libminato-luajit.so, the LuaJIT switch's guest half: the loader preloads it when it is among the system's libraries
// and resolves luaL_openlibs, by import and by dlsym alike, to the wrapper below wherever the library that defines it
// also defines luaJIT_setmode, writing the function it stands for into minato_luajit_original and luaJIT_setmode into
// minato_luajit_setmode. luaopen_jit turns LuaJIT's engine on while the libraries open, so the wrapper turns it off
// after them: luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF). The trace compiler then never records, and
// never asks for the executable memory that iOS does not give: LuaJIT reserves its machine code area at the first
// trace, not in luaopen_jit (lib_jit.c, lj_trace.c), so switching off after the libraries open is early enough. The
// switch is per state that goes through luaL_openlibs; a state whose libraries are opened one by one keeps its engine.
// Plain Lua has no luaJIT_setmode and is left alone.
#define LUAJIT_MODE_ENGINE 0
#define LUAJIT_MODE_OFF 0x0000

typedef void (*OpenLibs)(void *state);
typedef int (*SetMode)(void *state, int idx, int mode);

OpenLibs minato_luajit_original;
SetMode minato_luajit_setmode;

void minato_luajit_openlibs(void *state) {
    minato_luajit_original(state);
    if (minato_luajit_setmode != 0) minato_luajit_setmode(state, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
}
