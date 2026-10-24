#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

// Client-side Lua mod layer, mirror of ServerModManager's event bus. A client
// script registers handlers with the same net.on(name, fn) API and the engine
// dispatches client-scoped events into them (match start/leave, world seed,
// inbound custom events). Keeping the shared net.* table convention means one
// tutorial covers both sides; each process owns one Lua state.
class ClientModManager
{
public:
    ClientModManager();
    ~ClientModManager();

    // Loads (or reloads) a Lua script from disk. Returns true on success.
    bool LoadScript(const char* path);

    // Dispatches a named event to every Lua handler registered with
    // net.on(name, fn). Returns true if at least one handler ran.
    bool Emit(const char* eventName, const int* args, int argCount);

    // Reads an integer global from the loaded script (observability aid).
    int GetGlobalInt(const char* name) const;

    bool HasScript() const { return m_state != nullptr; }

private:
    void EnsureEventTable();
    static int LuaOnRegistration(lua_State* L);
    int PushEventTable(const char* eventName);
    bool InvokeHandlers(int handlersIndex, int nargs);

    lua_State* m_state;
};