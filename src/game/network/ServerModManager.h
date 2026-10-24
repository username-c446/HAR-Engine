#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "PlayerNetworkState.h"

#include <string>
#include <vector>

// Server-authoritative mod layer: lets server-side Lua scripts wrap the server's
// admission + state-validation decisions. A script may override the default
// outcomes through callbacks:
//   on_join(clientId)       -> returns bool (true = admit, default true)
//   on_validate(clientId, x, y, z, speed) -> returns bool (true = relay)
//   on_leave(clientId)
// If no script is loaded, behavior matches the built-in authoritative server.
//
// Beyond those three fixed hooks the class exposes an event bus: scripts
// register arbitrary handlers with net.on("my.event", fn) and the engine
// dispatches engine/game events to them with Emit(). This is the extensible
// path for mods that want to react to things the fixed hooks don't cover.
class ServerModManager
{
public:
    ServerModManager();
    ~ServerModManager();

    // Loads (or reloads) a Lua script from disk. Returns true on success.
    bool LoadScript(const char* path);

    // Hooks called by the server.
    bool OnPlayerJoin(int clientId);
    bool OnPlayerValidate(int clientId, const PlayerNetworkState& state);
    void OnPlayerLeave(int clientId);

    // ---- event bus -----------------------------------------------------

    // Dispatches a named event to all Lua handlers registered with
    // net.on(eventName, fn). Handlers receive the string key plus the typed
    // args passed here. Returns true if at least one handler ran.
    // Supported typed args (pushed in order): int (via argCount/args).
    bool Emit(const char* eventName, const int* args, int argCount);

    // Reads an integer global from the loaded script (e.g. a counter a handler
    // incremented). Returns 0 if absent or not a number. Test/observability aid.
    int GetGlobalInt(const char* name) const;

    bool HasScript() const { return m_state != nullptr; }

private:
    // Pushes the named global Lua function onto the stack; returns true if it
    // exists and is callable (function left on top of the stack on success).
    bool PushFunction(const char* name);
    // Registers the table that backs net.on()/net.broadcast() (created lazily).
    void EnsureEventTable();
    // Body of the net.on() Lua function (see LuaOnRegistration).
    static int LuaOnRegistration(lua_State* L);
    // Looks up `events[eventName]` (a table of handler functions) and returns
    // its length; leaves the table on the stack. Returns -1 if absent.
    int PushEventTable(const char* eventName);
    // Runs nargs functions from the table on top of the stack, each with the
    // args already sitting above `firstArgIndex`. Used by Emit().
    bool InvokeHandlers(int firstArgIndex, int nargs);

    lua_State* m_state;
};