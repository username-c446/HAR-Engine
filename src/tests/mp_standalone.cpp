// Headless multiplayer test client.
//
// Runs the engine client (net::Session / GamePlayerBridge, ENet, no game data)
// against a real server over sockets: a hosting game or the dedicated server
// (src/server, har_server). The in-process unit tests cannot cover that.
//
// Usage:
//   mp_standalone join <hostIp> [port] [--password <pw>]
//   mp_standalone genui [out.p3d]
//
//   join  -> connects, sends a fake moving state ~10/s, prints the remote
//            players and the late-join world snapshot it receives.
//   genui -> writes a Scrooby lobby screen with P3DWriter.
//
// The dedicated server itself is the har_server product (no fakes).
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include "p3d/p3dwriter.h"

#include "mp_network_fakes.h"

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
        printf("usage:\n  mp_standalone join <hostIp> [port] [--password <pw>]\n"
               "  mp_standalone genui [out.p3d]\n"
               "  (default port 3737; the dedicated server is har_server)\n");
        return 1;
    }

    const char* mode = argv[1];
    unsigned short port = 3737;
    const char* password = NULL;

    // Parse [port] [--password pw].
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            password = argv[i + 1];
            ++i;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            port = static_cast<unsigned short>(atoi(argv[i]));
        }
    }

    if (strcmp(mode, "join") == 0) {
        const char* ip = argv[2];
        net::Session client;
        if (password != NULL && password[0] != '\0') client.SetPassword(password);

        // Publish a slowly-moving state so the server sees us as a live player.
        g_fake.st.x = 0.0f; g_fake.st.y = 0.0f; g_fake.st.z = 0.0f; g_fake.st.rotY = 0.0f;
        g_fake.st.inVehicle = 0; g_fake.st.vehicleModel[0] = '\0';
        g_fake.ready = true;
        net::GamePlayerBridge bridge;
        bridge.SetTypedLocalProvider(&FakeLocalState);
        // Attach a remote repo so we can PRINT what we receive from other
        // clients. Without this the join loop only sends; it never proves the
        // engine delivers a peer's state to the other peers on the wire.
        RemotePlayerManager remote;
        bridge.SetRemotePlayers(&remote);
        bridge.AttachToSession(&client);
        // Late-join check: print the world snapshot as it arrives.
        size_t snapshotEntries = 0;
        client.SetWorldStateSink([&snapshotEntries](const net::WorldStateEntry& e, net::WorldStateReason reason, net::PeerId) {
            if (reason == net::WSR_SNAPSHOT) ++snapshotEntries;
            else printf("[join] world-state live obj=%016llx ch=%u\n", (unsigned long long)e.key.object, (unsigned)e.key.channel);
        });
        client.SetWorldSyncedFn([&snapshotEntries]() {
            printf("[join] world synced: %u snapshot entries\n", (unsigned)snapshotEntries);
            fflush(stdout);
        });

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
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // do not spin a core
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
                // Log remote players the engine delivered to us (i.e. OTHER
                // clients/servers visible through the relay). Proves two peers
                // actually see each other's replicated state over the wire.
                remote.ForEachPlayer(
                    [](int pid, const PlayerNetworkState& st, void*) -> bool {
                        printf("[join] remote state from peer %d: x=%.2f z=%.2f\n",
                               pid, st.x, st.z);
                        return true;
                    },
                    NULL);
                if (remote.GetPlayerCount() > 0 && client.GetWorldStateMirror().IsSynced()) break;
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