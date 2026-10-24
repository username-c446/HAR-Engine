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
//              [--log <file>] [--verbose]
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
               "                  [--log <file>] [--verbose]\n"
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
    Log("[server] %s, pvp %s\n", core.HasPassword() ? "password protected" : "open session",
        options.pvp ? "on" : "off");
    Log("[server] server mods loaded: %u of %u\n", static_cast<unsigned>(rules.ModCount()),
        static_cast<unsigned>(options.modScripts.size()));

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    size_t lastPlayers = 0;
    net::ServerCore::Stats lastStats;
    while (g_running)
    {
        core.Update();
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
