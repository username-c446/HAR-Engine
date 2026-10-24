#pragma once

#include "../netengine/ServerCore.h"
#include "PlayerMovementValidator.h"
#include "ServerModManager.h"

#include <memory>
#include <string>
#include <vector>

// The game's server rules, applied to a net::ServerCore. One class for both
// kinds of server: the hosting game attaches it to its Session's embedded
// core, the dedicated server product to its standalone core — so a match
// behaves the same whoever runs the server.
//
// It installs:
//   - the economy counters (EconomyConfig.h);
//   - the movement validator (PlayerMovementValidator) chained with the server
//     Lua mods' on_validate;
//   - the mods' on_join as the admission filter, on_leave and the
//     "player.joined" / "player.left" events;
//   - the session options (PvP).
class GameServer
{
public:
    struct Options
    {
        bool allowEconomyCheats = false;
        bool pvp = false;
        std::vector<std::string> modScripts;          // server Lua mods to load
        PlayerMovementValidator::Limits movement;
    };

    GameServer();
    ~GameServer();

    // Configures `core` (before or after it starts). The GameServer must outlive
    // the attachment; Detach() (or the destructor) removes every hook.
    void Attach(net::ServerCore& core, const Options& options);
    void Detach();
    bool IsAttached() const { return m_core != nullptr; }

    // Changes PvP for the running match (broadcast to every client).
    void SetPvp(bool enabled);

    PlayerMovementValidator& Movement() { return m_movement; }
    // Loaded mods (one manager per script).
    size_t ModCount() const { return m_mods.size(); }
    ServerModManager* Mod(size_t i) { return i < m_mods.size() ? m_mods[i].get() : nullptr; }

private:
    void EmitAll(const char* event, int arg);

    net::ServerCore* m_core = nullptr;
    PlayerMovementValidator m_movement;
    std::vector<std::unique_ptr<ServerModManager>> m_mods;
};
