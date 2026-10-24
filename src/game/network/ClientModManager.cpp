#include "ClientModManager.h"

#include <cstdio>
#include <cstring>

namespace
{
    const char* const kNetTable   = "net";
    const char* const kOnTable    = "handlers";
    const char* const kRegHandlers = "clmod_event_handlers";
}

ClientModManager::ClientModManager()
    : m_state(nullptr)
{
}

ClientModManager::~ClientModManager()
{
    if (m_state != nullptr) {
        lua_close(m_state);
    }
}

bool ClientModManager::LoadScript(const char* path)
{
    if (m_state == nullptr) {
        m_state = luaL_newstate();
        if (m_state == nullptr) return false;
        luaL_openlibs(m_state);
        EnsureEventTable();
    }

    if (luaL_loadfile(m_state, path) != LUA_OK) {
        const char* err = lua_tostring(m_state, -1);
        fprintf(stderr, "[ClientModManager] failed to load %s: %s\n", path, err ? err : "(unknown)");
        lua_pop(m_state, 1);
        return false;
    }
    if (lua_pcall(m_state, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(m_state, -1);
        fprintf(stderr, "[ClientModManager] script error in %s: %s\n", path, err ? err : "(unknown)");
        lua_pop(m_state, 1);
        return false;
    }
    return true;
}

void ClientModManager::EnsureEventTable()
{
    lua_newtable(m_state);
    lua_setglobal(m_state, kNetTable);

    lua_getglobal(m_state, kNetTable);
    lua_newtable(m_state);
    lua_setfield(m_state, -2, kOnTable);
    lua_pop(m_state, 1);

    lua_getglobal(m_state, kNetTable);
    lua_pushcfunction(m_state, &ClientModManager::LuaOnRegistration);
    lua_setfield(m_state, -2, "on");
    lua_pop(m_state, 1);

    lua_getglobal(m_state, kNetTable);
    lua_setfield(m_state, LUA_REGISTRYINDEX, kRegHandlers);
}

int ClientModManager::LuaOnRegistration(lua_State* L)
{
    if (lua_gettop(L) != 2 || !lua_isstring(L, 1) || !lua_isfunction(L, 2)) {
        lua_pushfstring(L, "usage: net.on(name, function)");
        return lua_error(L);
    }
    const char* name = lua_tostring(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, kRegHandlers); // net
    lua_getfield(L, -1, kOnTable);                    // handlers

    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name);   // handlers[name] = {}
    }

    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, static_cast<lua_Integer>(lua_rawlen(L, -2)) + 1);

    lua_pop(L, 3);   // list, handlers, net
    return 0;
}

int ClientModManager::PushEventTable(const char* eventName)
{
    if (m_state == nullptr) return -1;

    lua_getfield(m_state, LUA_REGISTRYINDEX, kRegHandlers);
    if (!lua_istable(m_state, -1)) { lua_pop(m_state, 1); return -1; }
    lua_getfield(m_state, -1, kOnTable);
    if (!lua_istable(m_state, -1)) { lua_pop(m_state, 2); return -1; }
    lua_getfield(m_state, -1, eventName);
    if (!lua_istable(m_state, -1)) { lua_pop(m_state, 3); return -1; }

    lua_remove(m_state, -2);
    lua_remove(m_state, -2);
    return static_cast<int>(lua_rawlen(m_state, -1));
}

bool ClientModManager::InvokeHandlers(int handlersIndex, int nargs)
{
    if (!lua_istable(m_state, handlersIndex)) { lua_pop(m_state, nargs); return false; }
    bool any = false;
    lua_Integer count = lua_rawlen(m_state, handlersIndex);
    for (lua_Integer i = 1; i <= count; ++i)
    {
        lua_rawgeti(m_state, handlersIndex, i);
        if (!lua_isfunction(m_state, -1)) { lua_pop(m_state, 1); continue; }
        for (int a = 0; a < nargs; ++a)
            lua_pushvalue(m_state, handlersIndex + 1 + a);

        if (lua_pcall(m_state, nargs, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(m_state, -1);
            fprintf(stderr, "[ClientModManager] event handler error: %s\n", err ? err : "(unknown)");
            lua_pop(m_state, 1);
        }
        else
        {
            any = true;
        }
    }
    lua_pop(m_state, nargs + 1);
    return any;
}

bool ClientModManager::Emit(const char* eventName, const int* args, int argCount)
{
    if (m_state == nullptr) return false;

    int count = PushEventTable(eventName);
    if (count <= 0) return false;
    int handlersIndex = lua_gettop(m_state);

    for (int i = 0; i < argCount; ++i)
        lua_pushinteger(m_state, args[i]);

    return InvokeHandlers(handlersIndex, argCount);
}

int ClientModManager::GetGlobalInt(const char* name) const
{
    if (m_state == nullptr) return 0;
    lua_getglobal(m_state, name);
    if (!lua_isnumber(m_state, -1)) { lua_pop(m_state, 1); return 0; }
    int v = static_cast<int>(lua_tointeger(m_state, -1));
    lua_pop(m_state, 1);
    return v;
}