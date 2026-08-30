/* SPDX-License-Identifier: GPL-3.0-only */

#include "lua.h"

#include <sapote/runtime.h>

#include <stdint.h>
#include <stdio.h>

static uint64_t startup_started;

void sapote_application_entry_probe(void);
void __real_luaL_openlibs(lua_State *state);
void __wrap_luaL_openlibs(lua_State *state);

void sapote_application_entry_probe(void)
{
    startup_started = sapote_monotonic_ns();
}

void __wrap_luaL_openlibs(lua_State *state)
{
    __real_luaL_openlibs(state);
    printf("SAPOTE PERF lua startup_ns=%llu\n",
        (unsigned long long)(sapote_monotonic_ns() - startup_started));
}
