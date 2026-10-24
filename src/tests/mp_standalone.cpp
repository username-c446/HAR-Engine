// Headless multiplayer test client.
//
// Runs the engine client (net::Session / GamePlayerBridge, ENet, no game data)
// against a real server over sockets: a hosting game or the dedicated server
// (src/server, har_server). The in-process unit tests cannot cover that.
//
// Usage:
//   mp_standalone join <hostIp> [port] [--password <pw>]
//   mp_standalone bots <count> <hostIp> [port] [--password <pw>] [--seconds <s>]
//                 [--groups <g>] [--spread <m>] [--interiors <k>] [--entities <e>]
//   mp_standalone genui [out.p3d]
//
//   join  -> connects, sends a fake moving state ~10/s, prints the remote
//            players and the late-join world snapshot it receives.
//   bots  -> load test: <count> headless players in one process, each its own
//            engine client (Session / ENet peer) streaming at the game's
//            20 Hz while circling. They are split into <g> groups <m> metres
//            apart, the first <k> are inside an interior (another context),
//            and each owns <e> streamed traffic entities. Run har_server with
//            --stats to read the server's traffic under that load.
//   genui -> writes a Scrooby lobby screen with P3DWriter.
//
// The dedicated server itself is the har_server product (no fakes).
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>
#include "p3d/p3dwriter.h"

#include "mp_network_fakes.h"
#include "../game/network/PlayerPresence.h"
#include "../game/network/NetEntityManager.h"

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

// One load-test player: a full engine client with its own ENet peer.
struct Bot : public net::ILocalStateProvider, public net::IRemoteStateSink
{
    net::Session session;
    PlayerNetworkState st = {};
    float cx = 0.0f, cz = 0.0f, radius = 10.0f, phase = 0.0f;
    bool  interior = false;
    int   entities = 0;
    bool  published = false;
    uint64_t statesReceived = 0;
    uint64_t entityPackets = 0;

    bool GetLocalState(uint8_t* out, uint16_t capacity, uint16_t& size) override
    {
        if (capacity < PlayerNetworkState::MAX_SERIALIZED_SIZE) return false;
        int n = st.Serialize(reinterpret_cast<char*>(out));
        if (n <= 0) return false;
        size = static_cast<uint16_t>(n);
        return true;
    }
    void ApplyRemoteState(net::PeerId, const uint8_t*, uint16_t, uint32_t) override { ++statesReceived; }
    void OnPeerLeft(net::PeerId) override {}

    // Circling at 5 m/s (well inside the server's on-foot limit).
    void Move(float seconds)
    {
        const float kSpeed = 5.0f;
        float a = phase + kSpeed / radius * seconds;
        st.x = cx + radius * std::cos(a);
        st.z = cz + radius * std::sin(a);
        st.y = 0.0f;
        st.rotY = a + 1.5707963f;
        st.facingAngle = static_cast<uint16_t>(std::fmod(a + 1.5707963f, 6.2831853f) * 1000.0f);
        st.motionAngle = st.facingAngle;
        st.speedFactor = 128;
    }

    // Context record and owned entities, once welcomed.
    void Publish()
    {
        net::PeerId self = session.GetLocalPeerId();
        net::WorldStateOp op;
        op.key.object = net::MakePlayerObjectId(self);
        op.key.channel = net::WOC_PLAYER_CONTEXT;
        op.kind = net::WSO_SET;
        op.policy = net::WSP_OWNER;
        op.flags = net::WSF_ERASE_ON_OWNER_LEAVE;
        net::PlayerContext ctx;
        ctx.level = 1;
        ctx.zone = interior ? 0x1234u : 0u;
        if (ctx.Encode(op.value)) session.RequestWorldState(op);

        for (int k = 1; k <= entities; ++k)
        {
            net::NetEntitySpawn spawn;
            spawn.kind = net::NEK_TRAFFIC_CAR;
            std::strncpy(spawn.model, "compactA", sizeof(spawn.model) - 1);
            spawn.x = cx;
            spawn.z = cz;
            net::WorldStateOp e;
            e.key.object = net::MakeDynamicEntityId(self, static_cast<uint32_t>(k));
            e.key.channel = net::WS_CHANNEL_ENTITY;
            e.kind = net::WSO_SET;
            e.policy = net::WSP_OWNER;
            e.flags = net::WSF_ERASE_ON_OWNER_LEAVE;
            if (spawn.Encode(e.value)) session.RequestWorldState(e);
        }
        published = true;
    }

    // The owned traffic circles the bot, streamed like the game does (10 Hz).
    void StreamEntities(float seconds)
    {
        if (entities == 0) return;
        net::PeerId self = session.GetLocalPeerId();
        std::vector<net::EntityStateRecord> records;
        for (int k = 1; k <= entities; ++k)
        {
            float a = phase + 0.5f * k + 0.3f * seconds;
            net::NetEntityMotion m;
            m.x = cx + (radius + 15.0f) * std::cos(a);
            m.z = cz + (radius + 15.0f) * std::sin(a);
            m.yaw = a;
            m.speedKmh = 40;
            net::EntityStateRecord rec;
            m.Encode(rec, net::MakeDynamicEntityId(self, static_cast<uint32_t>(k)));
            records.push_back(rec);
        }
        if (session.SendEntityStates(records)) ++entityPackets;
    }
};

static int RunBots(int argc, char** argv)
{
    if (argc < 4) {
        printf("usage: mp_standalone bots <count> <hostIp> [port] [--password <pw>] [--seconds <s>]\n"
               "                      [--groups <g>] [--spread <m>] [--interiors <k>] [--entities <e>]\n");
        return 1;
    }
    int count = atoi(argv[2]);
    const char* ip = argv[3];
    unsigned short port = 3737;
    const char* password = NULL;
    int seconds = 20, groups = 1, interiors = 0, entities = 0;
    float spread = 0.0f;
    for (int i = 4; i < argc; ++i) {
        bool v = i + 1 < argc;
        if (strcmp(argv[i], "--password") == 0 && v) password = argv[++i];
        else if (strcmp(argv[i], "--seconds") == 0 && v) seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--groups") == 0 && v) groups = atoi(argv[++i]);
        else if (strcmp(argv[i], "--spread") == 0 && v) spread = static_cast<float>(atof(argv[++i]));
        else if (strcmp(argv[i], "--interiors") == 0 && v) interiors = atoi(argv[++i]);
        else if (strcmp(argv[i], "--entities") == 0 && v) entities = atoi(argv[++i]);
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') port = static_cast<unsigned short>(atoi(argv[i]));
    }
    if (count <= 0 || groups <= 0) return 1;

    std::vector<std::unique_ptr<Bot>> bots;
    for (int i = 0; i < count; ++i) {
        std::unique_ptr<Bot> b(new Bot());
        int g = i % groups;
        b->cx = g * spread;
        b->cz = 0.0f;
        b->radius = 8.0f + (i / groups) * 3.0f;
        b->phase = 0.7f * i;
        b->interior = i < interiors;
        b->entities = entities;
        b->Move(0.0f);
        if (password != NULL && password[0] != '\0') b->session.SetPassword(password);
        b->session.SetLocalStateProvider(b.get());
        b->session.SetRemoteStateSink(b.get());
        b->session.SetStateSendIntervalMs(50); // the game's player stream rate
        if (!b->session.Join(ip, port)) {
            printf("[bots] bot %d: connect failed to %s:%u\n", i, ip, port);
            return 2;
        }
        bots.push_back(std::move(b));
    }
    printf("[bots] %d bots -> %s:%u, %d group(s) %.0f m apart, %d in an interior, %d entities each, %d s\n",
           count, ip, port, groups, spread, interiors, entities, seconds);
    fflush(stdout);

    auto start = std::chrono::steady_clock::now();
    auto lastEntity = start;
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        float t = std::chrono::duration<float>(now - start).count();
        if (t >= seconds) break;
        bool entityTick = std::chrono::duration<float>(now - lastEntity).count() >= 0.1f;
        if (entityTick) lastEntity = now;
        for (auto& b : bots) {
            b->Move(t);
            b->session.Update();
            if (b->session.GetLocalPeerId() == 0) continue;
            if (!b->published) b->Publish();
            if (entityTick) b->StreamEntities(t);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    int connected = 0;
    uint64_t received = 0;
    for (auto& b : bots) {
        if (b->session.GetLocalPeerId() != 0) ++connected;
        received += b->statesReceived;
    }
    printf("[bots] connected %d/%d, remote player states received: %.1f per bot per second\n",
           connected, count, connected ? static_cast<double>(received) / connected / seconds : 0.0);
    for (int i = 0; i < count && i < 32; ++i)
        printf("[bots]   bot %d (group %d%s): %.1f states/s\n", i, i % groups,
               bots[i]->interior ? ", interior" : "", static_cast<double>(bots[i]->statesReceived) / seconds);
    fflush(stdout);
    for (auto& b : bots) b->session.Leave();
    return connected == count ? 0 : 3;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage:\n  mp_standalone join <hostIp> [port] [--password <pw>]\n"
               "  mp_standalone bots <count> <hostIp> [port] [--seconds s] [--groups g] [--spread m]\n"
               "                     [--interiors k] [--entities e]\n"
               "  mp_standalone genui [out.p3d]\n"
               "  (default port 3737; the dedicated server is har_server)\n");
        return 1;
    }

    const char* mode = argv[1];
    if (strcmp(mode, "bots") == 0) return RunBots(argc, argv);
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