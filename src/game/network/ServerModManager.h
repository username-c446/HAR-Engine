#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include "PlayerNetworkState.h"

// Server-authoritative mod layer: lets server-side Lua scripts wrap the server's
// admission + state-validation decisions. A script may override the default
// outcomes through callbacks:
//   on_join(clientId)       -> returns bool (true = admit, default true)
//   on_validate(clientId, x, y, z, speed) -> returns bool (true = relay)
//   on_leave(clientId)
// If no script is loaded, behavior matches the built-in authoritative server.
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

    bool HasScript() const { return m_state != nullptr; }

private:
    // Pushes the named global Lua function onto the stack; returns true if it
    // exists and is callable (function left on top of the stack on success).
    bool PushFunction(const char* name);

    lua_State* m_state;
};