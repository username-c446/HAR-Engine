// HAR dedicated server.
//
// The same authoritative server a hosting game runs in-process (net::ServerCore
// + the game's rules in GameServer: economy, movement validation, PvP option,
// server Lua mods), as a console process with no game, window or assets. No
// player slot is taken by the server itself.
//
// Usage:
//   har_server [port] [--password <pw>] [--max <players>] [--mod <lua>]...
//              [--pvp] [--cheats] [--game-version <n>] [--seed <n>]
//              [--no-interest] [--stats <seconds>] [--log <file>] [--verbose]
//
// --stats prints the traffic every N seconds: total and per packet id, and
// what interest management kept off the wire. --no-interest sends every
// stream sample to every player (for comparison).
#include "ServerCore.h"
#include "GameServer.h"

#include <atomic>
#include <cstdarg>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace
{
    std::atomic<bool> g_running(true);

    void OnSignal(int) { g_running = false; }

    void Usage()
    {
        printf("usage: har_server [port] [--password <pw>] [--max <players>] [--mod <lua>]...\n"
               "                  [--pvp] [--cheats] [--game-version <n>] [--seed <n>]\n"
               "                  [--no-interest] [--stats <seconds>] [--log <file>] [--verbose]\n"
               "  default port 3737; an empty password = open session\n");
    }

    FILE* g_log = nullptr;

    // One line to the console and, with --log, to the log file.
    void Log(const char* fmt, ...)
    {
        char line[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        fputs(line, stdout);
        fflush(stdout);
        if (g_log != nullptr)
        {
            fputs(line, g_log);
            fflush(g_log);
        }
    }

    const char* PacketName(net::PacketId id)
    {
        switch (id)
        {
        case net::PID_HELLO: return "HELLO";
        case net::PID_WELCOME: return "WELCOME";
        case net::PID_REFUSE: return "REFUSE";
        case net::PID_PLAYER_LIST: return "PLAYER_LIST";
        case net::PID_PING: return "PING";
        case net::PID_PONG: return "PONG";
        case net::PID_WORLD_SEED: return "WORLD_SEED";
        case net::PID_SESSION_INFO: return "SESSION_INFO";
        case net::PID_STATE_CORRECTION: return "STATE_CORRECTION";
        case net::PID_WS_REQUEST: return "WS_REQUEST";
        case net::PID_WS_UPDATE: return "WS_UPDATE";
        case net::PID_WS_REJECT: return "WS_REJECT";
        case net::PID_WS_ERASE: return "WS_ERASE";
        case net::PID_WS_SNAPSHOT: return "WS_SNAPSHOT";
        case net::PID_WS_SNAPSHOT_END: return "WS_SNAPSHOT_END";
        case net::PID_LEDGER_REQUEST: return "LEDGER_REQUEST";
        case net::PID_LEDGER_RESULT: return "LEDGER_RESULT";
        case net::PID_PLAYER_STATE: return "PLAYER_STATE";
        case net::PID_VEHICLE_STATE: return "VEHICLE_STATE";
        case net::PID_MISSION_STATE: return "MISSION_STATE";
        case net::PID_ENTITY_STATE: return "ENTITY_STATE";
        case net::PID_ENTITY_EVENT: return "ENTITY_EVENT";
        case net::PID_PLAYER_EVENT: return "PLAYER_EVENT";
        case net::PID_USER: return "USER";
        case net::PID_USER_STREAM: return "USER_STREAM";
        default: return "?";
        }
    }

    double Kbps(uint64_t bytes, double seconds) { return seconds > 0.0 ? bytes * 8.0 / 1000.0 / seconds : 0.0; }

    // One report of what changed since `prev` (then `prev` = now).
    void ReportTraffic(const net::ServerCore& core, net::TrafficStats& prev,
                       net::ServerCore::Stats& prevStats, double seconds)
    {
        const net::TrafficStats* now = core.GetTraffic();
        if (now == nullptr) return;
        const net::ServerCore::Stats& st = core.GetStats();
        uint64_t outBytes = 0, inBytes = 0;
        for (const auto& kv : now->sent) outBytes += kv.second.bytes;
        for (const auto& kv : now->received) inBytes += kv.second.bytes;
        uint64_t prevOut = 0, prevIn = 0;
        for (const auto& kv : prev.sent) prevOut += kv.second.bytes;
        for (const auto& kv : prev.received) prevIn += kv.second.bytes;
        Log("[stats] players %u | out %.1f kbit/s (wire %.1f) | in %.1f kbit/s (wire %.1f) | "
            "not sent: %.0f player samples/s, %.0f entity records/s\n",
            static_cast<unsigned>(core.GetAdmittedPeers().size()),
            Kbps(outBytes - prevOut, seconds), Kbps(now->wireSent.bytes - prev.wireSent.bytes, seconds),
            Kbps(inBytes - prevIn, seconds), Kbps(now->wireReceived.bytes - prev.wireReceived.bytes, seconds),
            (st.samplesFiltered - prevStats.samplesFiltered) / seconds,
            (st.entityRecordsFiltered - prevStats.entityRecordsFiltered) / seconds);
        // Link quality across players: worst RTT / throttle / loss.
        uint32_t maxRtt = 0, minThrottle = 32, throttled = 0;
        float maxLoss = 0.0f;
        for (net::PeerId peer : core.GetAdmittedPeers())
        {
            net::ServerEndpoint::PeerLink link;
            if (!core.GetPeerLink(peer, link)) continue;
            if (link.rttMs > maxRtt) maxRtt = link.rttMs;
            if (link.throttle < minThrottle) minThrottle = link.throttle;
            if (link.throttle < 32) ++throttled;
            if (link.packetLoss > maxLoss) maxLoss = link.packetLoss;
        }
        Log("[stats]   links: max rtt %u ms, throttled %u (min %u/32), max loss %.1f%%\n",
            maxRtt, throttled, minThrottle, maxLoss * 100.0f);
        for (const auto& kv : now->sent)
        {
            auto p = prev.sent.find(kv.first);
            uint64_t pk = kv.second.packets - (p != prev.sent.end() ? p->second.packets : 0);
            uint64_t by = kv.second.bytes - (p != prev.sent.end() ? p->second.bytes : 0);
            if (pk == 0) continue;
            Log("[stats]   out 0x%04x %-16s %8.1f pkt/s %8.1f kbit/s %5.1f B/pkt\n", (unsigned)kv.first,
                PacketName(kv.first), pk / seconds, Kbps(by, seconds), static_cast<double>(by) / pk);
        }
        prev = *now;
        prevStats = st;
    }
}

int main(int argc, char** argv)
{
    uint16_t port = 3737;
    const char* password = nullptr;
    unsigned maxPlayers = 0;
    uint32_t gameVersion = 0;
    uint32_t seed = 0;
    const char* logPath = nullptr;
    bool verbose = false;
    unsigned statsSeconds = 0;
    GameServer::Options options;

    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        bool hasValue = i + 1 < argc;
        if (strcmp(a, "--password") == 0 && hasValue) password = argv[++i];
        else if (strcmp(a, "--max") == 0 && hasValue) maxPlayers = static_cast<unsigned>(atoi(argv[++i]));
        else if (strcmp(a, "--mod") == 0 && hasValue) options.modScripts.push_back(argv[++i]);
        else if (strcmp(a, "--game-version") == 0 && hasValue) gameVersion = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        else if (strcmp(a, "--seed") == 0 && hasValue) seed = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
        else if (strcmp(a, "--log") == 0 && hasValue) logPath = argv[++i];
        else if (strcmp(a, "--stats") == 0 && hasValue) statsSeconds = static_cast<unsigned>(atoi(argv[++i]));
        else if (strcmp(a, "--no-interest") == 0) options.interest.enabled = false;
        else if (strcmp(a, "--pvp") == 0) options.pvp = true;
        else if (strcmp(a, "--cheats") == 0) options.allowEconomyCheats = true;
        else if (strcmp(a, "--verbose") == 0) verbose = true;
        else if (a[0] >= '0' && a[0] <= '9') port = static_cast<uint16_t>(atoi(a));
        else
        {
            Usage();
            return 1;
        }
    }

    if (logPath != nullptr)
    {
        g_log = fopen(logPath, "a");
        if (g_log == nullptr) printf("[server] WARNING: cannot open log file %s\n", logPath);
    }

    net::ServerCore core;
    core.SetPassword(password);
    if (maxPlayers > 0) core.SetMaxPeers(maxPlayers);
    core.SetGameVersion(gameVersion);

    GameServer rules;
    rules.Attach(core, options);

    if (seed == 0)
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        seed = (static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count())
                ^ 0x5EED0000u) & 0x7FFFFFFFu;
        if (seed == 0) seed = 0x5EED0000u;
    }
    if (!core.Start(port, seed))
    {
        Log("[server] cannot listen on port %u\n", (unsigned)port);
        return 2;
    }

    if (verbose)
    {
        core.SetWorldChangeObserver([](const net::WorldStateEntry& e, bool erased, net::PeerId origin)
        {
            Log("[server] world %s obj=%016llx ch=%u owner=%u origin=%u\n",
                erased ? "erase" : "set", (unsigned long long)e.key.object, (unsigned)e.key.channel,
                (unsigned)e.owner, (unsigned)origin);
        });
    }

    Log("[server] listening on port %u (slots %u, seed %08x)\n", (unsigned)port,
        static_cast<unsigned>(core.GetMaxPeers()), seed);
    Log("[server] %s, pvp %s, interest management %s\n", core.HasPassword() ? "password protected" : "open session",
        options.pvp ? "on" : "off", options.interest.enabled ? "on" : "off");
    Log("[server] server mods loaded: %u of %u\n", static_cast<unsigned>(rules.ModCount()),
        static_cast<unsigned>(options.modScripts.size()));

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    size_t lastPlayers = 0;
    net::ServerCore::Stats lastStats;
    net::TrafficStats reportTraffic;
    net::ServerCore::Stats reportStats;
    auto lastReport = std::chrono::steady_clock::now();
    while (g_running)
    {
        core.Update();
        if (statsSeconds > 0)
        {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - lastReport).count();
            if (elapsed >= statsSeconds)
            {
                ReportTraffic(core, reportTraffic, reportStats, elapsed);
                lastReport = now;
            }
        }
        size_t players = core.GetAdmittedPeers().size();
        if (players != lastPlayers)
        {
            Log("[server] players in match: %u\n", static_cast<unsigned>(players));
            lastPlayers = players;
        }
        const net::ServerCore::Stats& st = core.GetStats();
        if (st.refused != lastStats.refused)
            Log("[server] refused handshakes: %u\n", st.refused);
        if (st.floodKicks != lastStats.floodKicks)
            Log("[server] flood kicks: %u\n", st.floodKicks);
        if (st.corrections != lastStats.corrections && verbose)
            Log("[server] movement corrections: %u\n", st.corrections);
        lastStats = st;
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // do not spin a core
    }

    Log("[server] shutting down\n");
    rules.Detach();
    core.Stop();
    if (g_log != nullptr) fclose(g_log);
    return 0;
}
