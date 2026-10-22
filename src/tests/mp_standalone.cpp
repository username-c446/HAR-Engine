// Standalone multiplayer headless test stand / dedicated server.
//
// Runs the real NetworkServer/NetworkClient (ENet, no game data needed) between
// two live processes, so the network layer is exercised over actual sockets —
// the thing the in-process unit tests cannot cover.
//
// Usage:
//   mp_standalone server [port] [--mod <lua-path>]
//   mp_standalone host [port]
//   mp_standalone join <hostIp> [port]
//
//   server -> pure console dedicated server (no window, no gameplay). Loads a
//             server-side Lua mod if --mod is given. Prints connect/disconnect.
//   host   -> starts a server, prints each connected client, and waits forever.
//   join   -> connects to the host, sends a fake moving state ~10/s, and applies
//             the server-authoritative player list.
//
// The mock in mp_network_fakes.h supplies the world hooks the game normally
// wires up (local state provider + remote player world sink), so this builds
// and runs standalone.
#include <cstdio>
#include <cstring>
#include "p3d/p3dwriter.h"

#include "mp_network_fakes.h"

// Small ENet-free helper to snapshot how many remote players the server has.
static int ServerRemotePlayerCount(const NetworkServer& server)
{
    return server.GetRemotePlayers().GetPlayerCount();
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage:\n  mp_standalone server [port] [--mod <lua>] [--password <pw>] [--max <slots>] [--log <file>]\n"
               "  mp_standalone host [port] [--password <pw>] [--max <slots>] [--log <file>]\n"
               "  mp_standalone join <hostIp> [port] [--password <pw>]\n"
               "  mp_standalone genui [out.p3d]\n"
               "  (default port 3737)\n");
        return 1;
    }

    const char* mode = argv[1];
    unsigned short port = 3737;
    const char* modPath = NULL;
    const char* password = NULL;
    unsigned short maxPlayers = 0;
    const char* logPath = NULL;

    // Parse [port] [--mod path] [--password pw] [--max n] [--log path]
    // generically for server/host modes.
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
            modPath = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            password = argv[i + 1];
            ++i;
        } else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            maxPlayers = static_cast<unsigned short>(atoi(argv[i + 1]));
            ++i;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logPath = argv[i + 1];
            ++i;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            port = static_cast<unsigned short>(atoi(argv[i]));
        }
    }

    if (strcmp(mode, "server") == 0 || strcmp(mode, "host") == 0) {
        NetworkServer server;
        if (maxPlayers > 0) server.SetMaxPlayers(maxPlayers);
        if (password != NULL && password[0] != '\0') server.SetPassword(password);
        if (!server.Start(port)) {
            printf("[%s] failed to start on port %u\n", mode, port);
            return 2;
        }
        if (modPath != NULL) {
            if (server.LoadServerMod(modPath)) {
                printf("[%s] loaded server mod: %s\n", mode, modPath);
            } else {
                printf("[%s] WARNING: failed to load server mod: %s\n", mode, modPath);
            }
        }
        fflush(stdout);
        printf("[%s] listening on %u. connect with mp_standalone join ...\n", mode, port);
        fflush(stdout);

        // Optional file log: write one line per notable server event.
        FILE* logfile = NULL;
        if (logPath != NULL && logPath[0] != '\0') {
            logfile = fopen(logPath, "a");
            if (logfile != NULL) {
                fprintf(logfile, "=== mp dedicated server started on port %u%s%s ===\n",
                        port,
                        password != NULL && password[0] != '\0' ? " (password protected)" : "",
                        maxPlayers > 0 ? " (slots configured)" : "");
                fflush(logfile);
            } else {
                printf("[%s] WARNING: cannot open log file: %s\n", mode, logPath);
            }
        }

        int lastCount = 0;
        for (;;) {
            server.Update();
            int count = ServerRemotePlayerCount(server);
            if (count != lastCount) {
                printf("[%s] clients in world: %d\n", mode, count);
                fflush(stdout);
                if (logfile != NULL) {
                    fprintf(logfile, "clients:%d\n", count);
                    fflush(logfile);
                }
                lastCount = count;
            }
        }
    }
    else if (strcmp(mode, "join") == 0) {
        const char* ip = argv[2];
        NetworkClient client;
        if (password != NULL && password[0] != '\0') client.SetPassword(password);
        if (!client.Connect(ip, port)) {
            printf("[join] connect failed to %s:%u\n", ip, port);
            return 2;
        }
        printf("[join] connecting to %s:%u ...\n", ip, port);
        fflush(stdout);
        bool announced = false;
        for (;;) {
            client.Update();
            if (client.IsServerReady()) {
                if (!announced) {
                    printf("[join] SERVER READY, my client id = %u\n",
                           client.GetClientId());
                    fflush(stdout);
                    announced = true;
                }
                // Send a slowly moving signature once ready.
                static PlayerNetworkState st;
                st.x += 0.1f;
                st.inVehicle = 0;
                client.SendPlayerState(st);
            }
        }
    }
    else if (strcmp(mode, "genui") == 0) {
        // Generate a Scrooby .p3d lobby screen to disk using P3DWriter.
        const char* outPath = (argc >= 3) ? argv[2] : "lobby.p3d";
        P3DWriter w;
        w.BeginProject("mp_lobby", 640, 480, "pc", "art\\pages", "art\\res", "art\\screens");
        w.BeginScreen("mp_lobby_screen", 1, 0);
        w.EndScreen();
        w.BeginPage("mp_lobby_page", 1, 640, 480);
        w.BeginLayer("main", 1, true, true, 255);
        w.BeginGroup("title_grp", 1, 0);
        w.WriteText("Title", 17, 100, 40, 440, 40, 0, 0, 0xFFFFFFFF,
            0, 0.0f, "HudTextStyle", true, 0xFF000000, 1, 2, 0);
        w.AddTextHardCodedString("Multiplayer Lobby");
        w.EndText();
        w.EndGroup();
        w.BeginGroup("status_grp", 1, 0);
        w.WriteText("Status", 17, 100, 120, 440, 30, 0, 0, 0xFFFFFFFF,
            0, 0.0f, "HudTextStyle", true, 0xFF000000, 1, 2, 0);
        w.AddTextHardCodedString("Host a game to begin.");
        w.EndText();
        w.EndGroup();
        w.EndLayer();
        w.EndPage();
        w.EndProject();

        FILE* f = fopen(outPath, "wb");
        if (f == NULL) {
            printf("[genui] cannot open output file: %s\n", outPath);
            return 2;
        }
        const std::vector<char>& data = w.GetData();
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        printf("[genui] wrote %u bytes to %s\n", (unsigned)data.size(), outPath);
        return 0;
    }
    else {
        printf("unknown mode '%s'\n", mode);
        return 1;
    }
}