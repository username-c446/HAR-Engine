// Standalone multiplayer headless test stand / dedicated server.
//
// Runs the engine network layer (net::Session / GamePlayerBridge, ENet, no game
// data needed) between two live processes, so the network layer is exercised
// over actual sockets — the thing the in-process unit tests cannot cover.
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
// The mock in mp_network_fakes.h supplies the world hooks the engine normally
// wires up (local state provider + remote player world sink), so this builds
// and runs standalone.
#include <cstdio>
#include <cstring>
#include "p3d/p3dwriter.h"

#include "mp_network_fakes.h"
#include "ServerModManager.h"   // live server-side Lua event bus (--mod)

// Simple game-side typed local provider: a slowly-advancing fake position.
static struct {
    PlayerNetworkState st = {};
    bool ready = false;
} g_fake;

static bool FakeLocalState(PlayerNetworkState& out)
{
    if (!g_fake.ready) return false;
    out = g_fake.st;
    return true;
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
        net::Session server;
        if (password != NULL && password[0] != '\0') server.SetHostPassword(password);
        // Log inbound user-data events (PID_USER) so we can verify the open
        // custom-event channel over real sockets between two processes.
        server.SetCustomEventSink([](net::PeerId from, const uint8_t* d, uint16_t size) {
            printf("[%s] custom-event from peer %u, %u bytes, first=0x%02x\n",
                   (const char*) ( "host" ), (unsigned)from, (unsigned)size,
                   size ? d[0] : 0);
            fflush(stdout);
        });
        if (!server.StartHost(port, 0x5EED0000u | static_cast<uint32_t>(port))) {
            printf("[%s] failed to start on port %u\n", mode, port);
            return 2;
        }
        if (modPath != NULL) {
            // Live server-side Lua mod: load it into ServerModManager and, once
            // a client connects, emit a real event through the session loop so
            // the event bus is exercised in a running process (not just tests).
            ServerModManager liveMod;
            if (liveMod.LoadScript(modPath)) {
                printf("[%s] loaded Lua mod: %s\n", mode, modPath);
                fflush(stdout);
                server.SetJoinLeaveFn([&liveMod](net::PeerId p, bool joined) {
                    if (joined) {
                        int ev = static_cast<int>(p);
                        liveMod.Emit("player.joined", &ev, 1);
                    }
                });
            } else {
                printf("[%s] WARNING: failed to load Lua mod: %s\n", mode, modPath);
                fflush(stdout);
            }
        }
        else if (strcmp(mode, "server") != 0) { (void)0; }
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
            int count = server.GetPeerRoster().size();
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
        net::Session client;
        if (password != NULL && password[0] != '\0') client.SetPassword(password);

        // Publish a slowly-moving state so the server sees us as a live player.
        g_fake.st.x = 0.0f; g_fake.st.y = 0.0f; g_fake.st.z = 0.0f; g_fake.st.rotY = 0.0f;
        g_fake.st.inVehicle = 0; g_fake.st.vehicleModel[0] = '\0';
        g_fake.ready = true;
        net::GamePlayerBridge bridge;
        bridge.SetTypedLocalProvider(&FakeLocalState);
        bridge.AttachToSession(&client);

        if (!client.Join(ip, port)) {
            printf("[join] connect failed to %s:%u\n", ip, port);
            return 2;
        }
        printf("[join] connecting to %s:%u ...\n", ip, port);
        fflush(stdout);
        bool announced = false;
        int tick = 0;
        for (;;) {
            client.Update();
            if (client.GetLocalPeerId() != 0) {
                if (!announced) {
                    printf("[join] SERVER READY, my client id = %u\n",
                           client.GetLocalPeerId());
                    fflush(stdout);
                    announced = true;
                }
                // Send a moving signature once ready.
                if (++tick % 6 == 0) g_fake.st.x += 0.1f;
                // On the 5th tick, ship a custom user event to prove the open
                // PID_USER channel works over real sockets between processes.
                if (tick == 5) {
                    const uint8_t msg[3] = { 0xDE, 0xAD, 0x07 };
                    bool sent = client.SendCustomEvent(msg, sizeof(msg));
                    printf("[join] sent custom event: %s\n", sent ? "yes" : "no");
                    fflush(stdout);
                    if (!sent) break;
                }
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