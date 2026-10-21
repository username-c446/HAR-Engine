#include "NetworkConsoleCommands.h"

#include "NetworkMatchManager.h"

#include <console/console.h>
#include <cstdlib>

namespace
{
    void ConsoleHost(int argc, const char** argv)
    {
        // host [port]  -> start a dedicated server on the given port (default 3737).
        unsigned short port = 3737;
        if (argc >= 2) port = static_cast<unsigned short>(atoi(argv[1]));

        NetworkMatchManager* mgr = NetworkMatchManager::GetInstance();
        if (mgr == NULL) mgr = NetworkMatchManager::CreateInstance();

        if (mgr->IsInMatch())
        {
            GetConsole()->Printf("match: already in a match; use match leave first");
            return;
        }
        if (!mgr->StartHost(port))
        {
            GetConsole()->Printf("host: failed to start server on port %u", port);
            return;
        }
        GetConsole()->Printf("host: server listening on port %u", port);
    }

    void ConsoleJoin(int argc, const char** argv)
    {
        // join <host> [port]  -> connect to a host (default port 3737).
        if (argc < 2)
        {
            GetConsole()->Printf("join: usage join <hostIp> [port]");
            return;
        }
        const char* ip = argv[1];
        unsigned short port = 3737;
        if (argc >= 3) port = static_cast<unsigned short>(atoi(argv[2]));

        NetworkMatchManager* mgr = NetworkMatchManager::GetInstance();
        if (mgr == NULL) mgr = NetworkMatchManager::CreateInstance();

        if (mgr->IsInMatch())
        {
            GetConsole()->Printf("match: already in a match; use match leave first");
            return;
        }
        if (!mgr->JoinHost(ip, port))
        {
            GetConsole()->Printf("join: could not connect to %s:%u", ip, port);
            return;
        }
        GetConsole()->Printf("join: connecting to %s:%u ...", ip, port);
    }

    void ConsoleMatch(int argc, const char** argv)
    {
        // match [leave|status]
        if (argc < 2)
        {
            GetConsole()->Printf("match: usage match <leave|status>");
            return;
        }
        if (strcmp(argv[1], "leave") == 0)
        {
            NetworkMatchManager* mgr = NetworkMatchManager::GetInstance();
            if (mgr != NULL) mgr->Leave();
            GetConsole()->Printf("match: left");
        }
        else if (strcmp(argv[1], "status") == 0)
        {
            NetworkMatchManager* mgr = NetworkMatchManager::GetInstance();
            bool inMatch = mgr != NULL && mgr->IsInMatch();
            unsigned short clientId = mgr != NULL ? mgr->GetLocalClientId() : 0;
            GetConsole()->Printf("match: inMatch=%d clientId=%u", inMatch ? 1 : 0, clientId);
            if (mgr != NULL && mgr->IsInMatch())
            {
                GetConsole()->Printf("match: players (%d):",
                    mgr->GetRemotePlayers().GetPlayerCount());
                mgr->GetRemotePlayers().ForEachPlayer(
                    [](int id, const PlayerNetworkState&, void*) -> bool
                    {
                        GetConsole()->Printf("match:   player id=%d", id);
                        return true;
                    },
                    NULL);
            }
        }
    }

    bool s_registered = false;
}

void RegisterNetworkConsoleCommands()
{
    if (s_registered) return;
    s_registered = true;

    Console* console = GetConsole();
    if (console == NULL) return;
    console->AddFunction("host", ConsoleHost, "host [port] — start a multiplayer host (default 3737)", 0, 1);
    console->AddFunction("join", ConsoleJoin, "join <hostIp> [port] — connect to a host (default 3737)", 1, 2);
    console->AddFunction("match", ConsoleMatch, "match <leave|status> — manage the current match", 1, 1);
}