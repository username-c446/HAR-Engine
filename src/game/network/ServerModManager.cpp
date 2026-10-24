#include "ServerModManager.h"

#include <cstdio>
#include <cstring>

namespace
{
    // The global table name backing the mod event API. Scripts call
    //   net.on("player.joined", function(clientId) ... end)
    // and the engine dispatches events into it with Emit().
    const char* const kNetTable   = "net";
    const char* const kOnTable    = "handlers"; // net.handlers[eventName] = { fns }

    // Registry key under which the events table is stashed (private registry,
    // hidden from the script's global namespace).
    const char* const kRegHandlers = "smm_event_handlers";
}

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
        EnsureEventTable();
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

// Creates net.handlers in the script's global table, once. Called on first Lua
// state creation so scripts can call net.on() top-level.
void ServerModManager::EnsureEventTable()
{
    // net = {}  (global)
    lua_newtable(m_state);
    lua_setglobal(m_state, kNetTable);

    // net.handlers = {}  used as a per-event list of handler functions
    lua_getglobal(m_state, kNetTable);
    lua_newtable(m_state);
    lua_setfield(m_state, -2, kOnTable);
    lua_pop(m_state, 1);

    // net.on(eventName, fn): append fn to net.handlers[eventName].
    lua_getglobal(m_state, kNetTable);          // [net]
    lua_pushcfunction(m_state, &ServerModManager::LuaOnRegistration);
    lua_setfield(m_state, -2, "on");            // net.on
    lua_pop(m_state, 1);                        // []

    // Also stash a private reference so Emit() can find it without polluting
    // the user's globals lookup path. Registry: [kRegHandlers] = net table.
    lua_getglobal(m_state, kNetTable);
    lua_setfield(m_state, LUA_REGISTRYINDEX, kRegHandlers);
}

int ServerModManager::LuaOnRegistration(lua_State* L)
{
    // args: [eventName (string), fn (function)]
    if (lua_gettop(L) != 2 || !lua_isstring(L, 1) || !lua_isfunction(L, 2)) {
        lua_pushfstring(L, "usage: net.on(name, function)");
        return lua_error(L);
    }
    const char* name = lua_tostring(L, 1);

    // Walk to `handlers` and keep net + handlers on the stack so we can write
    // into handlers[name] later. Stack: [eventName, fn, net, handlers]
    lua_getfield(L, LUA_REGISTRYINDEX, kRegHandlers); // -> net
    lua_getfield(L, -1, kOnTable);                    // -> handlers

    // list = handlers[name]; create + store if absent.
    // Stack after getfield: [eventName, fn, net, handlers, list|nil]
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);          // drop nil
        lua_newtable(L);        // list = {}
        lua_pushvalue(L, -1);   // dup list
        lua_setfield(L, -3, name); // handlers[name] = {} (target = handlers)
    }
    // Stack now: [eventName, fn, net, handlers, list] (list always on top)

    // Append fn to list: list[#list+1] = fn
    lua_pushvalue(L, 2);                       // -> [...,list, fn]
    lua_rawseti(L, -2, static_cast<lua_Integer>(lua_rawlen(L, -2)) + 1);

    // Cleanup: pop list, handlers, net. Stack restored to [eventName, fn].
    lua_pop(L, 1); // list
    lua_pop(L, 1); // handlers
    lua_pop(L, 1); // net
    return 0;
}

// Looks up net.handlers[eventName]; returns that table's length (number of
// handlers) and leaves the table on top of the stack, or -1 if absent.
int ServerModManager::PushEventTable(const char* eventName)
{
    if (m_state == nullptr) return -1;

    lua_getfield(m_state, LUA_REGISTRYINDEX, kRegHandlers); // net table
    if (!lua_istable(m_state, -1)) { lua_pop(m_state, 1); return -1; }
    lua_getfield(m_state, -1, kOnTable);                    // net.handlers
    if (!lua_istable(m_state, -1)) { lua_pop(m_state, 2); return -1; }
    lua_getfield(m_state, -1, eventName);                   // net.handlers[name]
    if (!lua_istable(m_state, -1)) { lua_pop(m_state, 3); return -1; }

    // stack now: [net, handlers, list]; we keep only the list for invocation.
    lua_remove(m_state, -2);
    lua_remove(m_state, -2);
    return static_cast<int>(lua_rawlen(m_state, -1));
}

// Runs every function in the handlers table at absolute stack index
// `handlersIndex`. The nargs arguments live at handlersIndex+1..handlersIndex
// +nargs on the stack. Each handler is called as fn(arg0, ..., argN-1); the
// original args stay put and are re-copied per call because pcall consumes them.
// Cleans up the table + args. Returns true if at least one handler ran.
bool ServerModManager::InvokeHandlers(int handlersIndex, int nargs)
{
    if (!lua_istable(m_state, handlersIndex)) { lua_pop(m_state, nargs); return false; }
    bool any = false;
    lua_Integer count = lua_rawlen(m_state, handlersIndex);
    for (lua_Integer i = 1; i <= count; ++i)
    {
        // Push fn, then copies of the args above it in order. pcall expects the
        // function on the stack first, args above it in order.
        lua_rawgeti(m_state, handlersIndex, i);            // -> fn
        if (!lua_isfunction(m_state, -1)) { lua_pop(m_state, 1); continue; }
        for (int a = 0; a < nargs; ++a)
            lua_pushvalue(m_state, handlersIndex + 1 + a); // -> copies of args

        if (lua_pcall(m_state, nargs, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(m_state, -1);
            fprintf(stderr, "[ServerModManager] event handler error: %s\n", err ? err : "(unknown)");
            lua_pop(m_state, 1);
        }
        else
        {
            any = true;
        }
    }
    lua_pop(m_state, nargs + 1);   // handlers table + the original args
    return any;
}

bool ServerModManager::Emit(const char* eventName, const int* args, int argCount)
{
    if (m_state == nullptr) return false;

    int count = PushEventTable(eventName);
    if (count <= 0) return false;
    int handlersIndex = lua_gettop(m_state);   // handlers table is on top

    for (int i = 0; i < argCount; ++i)
        lua_pushinteger(m_state, args[i]);     // stack: [handlers, args...]

    return InvokeHandlers(handlersIndex, argCount);
}

int ServerModManager::GetGlobalInt(const char* name) const
{
    if (m_state == nullptr) return 0;
    lua_getglobal(m_state, name);
    if (!lua_isnumber(m_state, -1)) { lua_pop(m_state, 1); return 0; }
    int v = static_cast<int>(lua_tointeger(m_state, -1));
    lua_pop(m_state, 1);
    return v;
}