#include "GameServer.h"
#include "EconomyConfig.h"

#include <cstdio>

GameServer::GameServer() = default;
GameServer::~GameServer() { Detach(); }

void GameServer::Attach(net::ServerCore& core, const Options& options)
{
    Detach();
    m_core = &core;

    core.ConfigureCounter(economy::COUNTER_COINS, economy::MakeCoinConfig(options.allowEconomyCheats));

    m_mods.clear();
    for (const std::string& path : options.modScripts)
    {
        std::unique_ptr<ServerModManager> mod(new ServerModManager());
        if (mod->LoadScript(path.c_str()))
            m_mods.push_back(std::move(mod));
        else
            fprintf(stderr, "[GameServer] failed to load server mod: %s\n", path.c_str());
    }

    m_movement.SetLimits(options.movement);
    m_movement.SetExtraCheck([this](net::PeerId peer, const PlayerNetworkState& st, float speed)
    {
        for (auto& mod : m_mods)
            if (!mod->OnPlayerValidate(static_cast<int>(peer), st, speed)) return false;
        return true;
    });
    core.SetStateValidator(&m_movement);

    m_interest.Clear();
    m_interest.SetConfig(options.interest);
    m_interest.SetWorld(&core.GetWorldStateTable());
    core.SetInterestPolicy(&m_interest);

    core.SetAdmissionFilter([this](net::PeerId peer)
    {
        for (auto& mod : m_mods)
            if (!mod->OnPlayerJoin(static_cast<int>(peer))) return false;
        return true;
    });
    core.SetJoinLeaveFn([this](net::PeerId peer, bool joined)
    {
        if (!joined)
            for (auto& mod : m_mods) mod->OnPlayerLeave(static_cast<int>(peer));
        EmitAll(joined ? "player.joined" : "player.left", static_cast<int>(peer));
    });

    uint32_t opts = core.GetSessionOptions() & ~static_cast<uint32_t>(net::SO_PVP);
    if (options.pvp) opts |= net::SO_PVP;
    core.SetSessionOptions(opts);
}

void GameServer::Detach()
{
    if (m_core == nullptr) return;
    m_core->SetStateValidator(nullptr);
    m_core->SetInterestPolicy(nullptr);
    m_interest.SetWorld(nullptr);
    m_core->SetAdmissionFilter(nullptr);
    m_core->SetJoinLeaveFn(nullptr);
    m_core = nullptr;
    m_mods.clear();
}

void GameServer::SetPvp(bool enabled)
{
    if (m_core == nullptr) return;
    uint32_t opts = m_core->GetSessionOptions() & ~static_cast<uint32_t>(net::SO_PVP);
    if (enabled) opts |= net::SO_PVP;
    m_core->SetSessionOptions(opts);
}

void GameServer::EmitAll(const char* event, int arg)
{
    for (auto& mod : m_mods) mod->Emit(event, &arg, 1);
}
