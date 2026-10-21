#include "ServerModManager.h"

#include <cstdio>

ServerModManager::ServerModManager()
    : m_state(nullptr)
{
}

ServerModManager::~ServerModManager()
{
    if (m_state != nullptr) {
        lua_close(m_state);
    }
}

bool ServerModManager::LoadScript(const char* path)
{
    if (m_state == nullptr) {
        m_state = luaL_newstate();
        if (m_state == nullptr) return false;
        luaL_openlibs(m_state);
    }

    if (luaL_loadfile(m_state, path) != LUA_OK) {
        const char* err = lua_tostring(m_state, -1);
        fprintf(stderr, "[ServerModManager] failed to load %s: %s\n", path, err ? err : "(unknown)");
        lua_pop(m_state, 1);
        return false;
    }
    if (lua_pcall(m_state, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(m_state, -1);
        fprintf(stderr, "[ServerModManager] script error in %s: %s\n", path, err ? err : "(unknown)");
        lua_pop(m_state, 1);
        return false;
    }
    return true;
}

// Pushes the named global function; returns true if it exists and is callable.
// On success the function is left on top of the stack (caller pushes args above
// it and invokes). On failure the stack is restored.
bool ServerModManager::PushFunction(const char* name)
{
    if (m_state == nullptr) return false;
    lua_getglobal(m_state, name);
    if (!lua_isfunction(m_state, -1)) {
        lua_pop(m_state, 1);
        return false;
    }
    return true;
}

bool ServerModManager::OnPlayerJoin(int clientId)
{
    if (m_state == nullptr) return true; // no script -> allow
    if (!PushFunction("on_join")) return true;

    lua_pushinteger(m_state, clientId);   // stack: [func, arg]

    if (lua_pcall(m_state, 1, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(m_state, -1);
        fprintf(stderr, "[ServerModManager] lua error in on_join: %s\n", err ? err : "(unknown)");
        lua_pop(m_state, 1);
        return true; // script error -> fail open, allow
    }
    int result = lua_toboolean(m_state, -1);
    lua_pop(m_state, 1);
    return result != 0;
}

bool ServerModManager::OnPlayerValidate(int clientId, const PlayerNetworkState& state)
{
    if (m_state == nullptr) return true; // no script -> allow
    if (!PushFunction("on_validate")) return true;

    lua_pushinteger(m_state, clientId);   // stack: [func, arg...]
    lua_pushnumber(m_state, state.x);
    lua_pushnumber(m_state, state.y);
    lua_pushnumber(m_state, state.z);
    lua_pushnumber(m_state, 0.0f);        // scalar speed placeholder

    if (lua_pcall(m_state, 5, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(m_state, -1);
        fprintf(stderr, "[ServerModManager] lua error in on_validate: %s\n", err ? err : "(unknown)");
        lua_pop(m_state, 1);
        return true; // fail open
    }
    int result = lua_toboolean(m_state, -1);
    lua_pop(m_state, 1);
    return result != 0;
}

void ServerModManager::OnPlayerLeave(int clientId)
{
    if (m_state == nullptr) return;
    if (!PushFunction("on_leave")) return;

    lua_pushinteger(m_state, clientId);   // stack: [func, arg]
    lua_pcall(m_state, 1, 0, 0);          // ignore errors
}