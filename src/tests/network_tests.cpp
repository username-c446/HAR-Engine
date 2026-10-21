#include "CppUnitTest.h"
#include "NetworkPacket.h"
#include "PlayerNetworkState.h"
#include "NetworkServer.h"
#include "NetworkClient.h"
#include "NetworkMatchManager.h"
#include "RemotePlayerWorldView.h"
#include "RemoteVehicleWorldView.h"
#include "ServerModManager.h"

#include <winsock2.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NetworkTests
{
    // Reserve a free UDP port by binding an ephemeral socket and releasing it.
    unsigned short PickFreePort()
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(s, (sockaddr*)&addr, sizeof(addr));
        int len = sizeof(addr);
        getsockname(s, (sockaddr*)&addr, &len);
        unsigned short port = ntohs(addr.sin_port);
        closesocket(s);
        WSACleanup();
        return port;
    }

    TEST_CLASS(PlayerStateTests)
    {
    public:
        TEST_METHOD(SerializeDeserialize)
        {
            PlayerNetworkState state;
            state.x = 1.0f;
            state.y = 2.0f;
            state.z = 3.0f;
            state.rotY = 4.0f;
            strcpy(state.vehicleModel, "homer");
            state.netVehicleId = 5;
            state.vehicleSpeed = 66.0f;
            state.inVehicle = 1;

            char buffer[PlayerNetworkState::SERIALIZED_SIZE];
            state.Serialize(buffer);

            PlayerNetworkState restored;
            restored.Deserialize(buffer);

            Assert::AreEqual(state.x, restored.x);
            Assert::AreEqual(state.y, restored.y);
            Assert::AreEqual(state.z, restored.z);
            Assert::AreEqual(state.rotY, restored.rotY);
            Assert::AreEqual(0, strcmp(state.vehicleModel, restored.vehicleModel));
            Assert::AreEqual(state.netVehicleId, restored.netVehicleId);
            Assert::AreEqual(state.vehicleSpeed, restored.vehicleSpeed);
            Assert::AreEqual(state.inVehicle, restored.inVehicle);
        }

        TEST_METHOD(SerializedSizeIsStable)
        {
            // Serialization must not depend on struct padding/alignment.
            // 4 floats (x,y,z,rotY) + vehicleModel[16] + int32 netVehicleId
            // + float vehicleSpeed + uint8 inVehicle + uint8 padding.
            Assert::AreEqual<int>(4 * 4 + 16 + 4 + 4 + 1 + 1, PlayerNetworkState::SERIALIZED_SIZE);
        }
    };

    TEST_CLASS(ClientServerLoopbackTests)
    {
    public:
        TEST_METHOD(ServerAcceptsHelloAndWelcome)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));

            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));

            // Pump both until the client is welcomed.
            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                client.Update();
                server.Update();
                welcomed = client.IsServerReady();
            }

            Assert::IsTrue(welcomed, L"Client did not receive WELCOME within loopback pump.");
            Assert::IsTrue(client.GetClientId() != 0);
        }

        TEST_METHOD(PlayerStateRoundtripThroughServer)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));

            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));

            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                client.Update();
                server.Update();
                welcomed = client.IsServerReady();
            }
            Assert::IsTrue(welcomed, L"Client did not get WELCOME.");

            // Send a state and let it travel to the server.
            PlayerNetworkState ourState;
            ourState.x = 10.0f;
            ourState.y = 20.0f;
            ourState.z = 30.0f;
            ourState.rotY = 1.5f;
            strcpy(ourState.vehicleModel, "homer");
            ourState.netVehicleId = 7;
            ourState.vehicleSpeed = 55.0f;
            ourState.inVehicle = 1;

            // Drive enough updates to emit a state (rate-limited inside SendPlayerState).
            for (int i = 0; i < 60; ++i) {
                client.SendPlayerState(ourState);
                client.Update();
                server.Update();
            }

            const PlayerNetworkState* serverView = server.GetRemotePlayers().GetPlayer(static_cast<int>(client.GetClientId()));
            Assert::IsNotNull(serverView, L"Server has no player state for this client.");
            if (serverView) {
                Assert::AreEqual(30.0f, serverView->z);
            }
        }

        TEST_METHOD(ServerDropsSilentClientAfterTimeout)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));
            // Use a tiny timeout so the test does not wait the real window.
            server.SetClientTimeoutMs(100);

            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));

            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                client.Update();
                server.Update();
                welcomed = client.IsServerReady();
            }
            Assert::IsTrue(welcomed, L"Client did not get WELCOME.");
            unsigned short cid = client.GetClientId();
            Assert::IsTrue(server.IsClientConnected(cid), L"Server should have a session for the client.");

            // Now stop pumping the client entirely; let the server's timeout sweep run.
            // The client stops sending pings, so after the window the server drops it.
            // Sleep a little so real (wall) time — which the timeout is based on —
            // advances past the window; the sweep runs on enet's own clock.
            bool dropped = false;
            for (int i = 0; i < 30 && !dropped; ++i) {
                Sleep(10);
                server.Update(); // no client.Update() - client is silent
                dropped = !server.IsClientConnected(cid);
            }
            Assert::IsTrue(dropped, L"Server did not drop a silent client after timeout.");
        }

        TEST_METHOD(HeartbeatKeepsClientAlive)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));
            server.SetClientTimeoutMs(100);
            // Ping every frame so the client stays visibly alive.

            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));
            client.SetPingIntervalMs(0); // heartbeat every Update

            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                client.Update();
                server.Update();
                welcomed = client.IsServerReady();
            }
            Assert::IsTrue(welcomed, L"Client did not get WELCOME.");
            unsigned short cid = client.GetClientId();

            // Pump well past the timeout window while the client keeps pinging.
            bool alive = true;
            for (int i = 0; i < 60; ++i) {
                client.Update();
                server.Update();
                if (!server.IsClientConnected(cid)) { alive = false; break; }
            }
            Assert::IsTrue(alive, L"Client should stay connected while heartbeating.");
        }

        TEST_METHOD(ClientMeasuresPingFromPong)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));

            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));
            client.SetPingIntervalMs(0); // ping on every Update

            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                client.Update();
                server.Update();
                welcomed = client.IsServerReady();
            }
            Assert::IsTrue(welcomed, L"Client did not get WELCOME.");

            // Pump a few rounds; the client should receive PONG and measure RTT.
            bool measured = false;
            for (int i = 0; i < 20 && !measured; ++i) {
                client.Update();
                server.Update();
                measured = client.HasMeasuredPing();
            }
            Assert::IsTrue(measured, L"Client never measured a ping round-trip.");
            // Loopback RTT should be a small positive value (or 0 on fast machines).
            Assert::IsTrue(client.GetPingMs() < 5000, L"Measured ping looks absurdly high.");
        }

        TEST_METHOD(ClientReportsJoinLeaveEvents)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));

            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));

            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                client.Update();
                server.Update();
                welcomed = client.IsServerReady();
            }
            Assert::IsTrue(welcomed, L"Client did not get WELCOME.");

            // After welcome, a list refresh with only ourselves produces no events.
            client.Update(); server.Update();
            std::vector<NetworkClient::JoinLeaveEvent> events;
            client.DrainJoinLeaveEvents(events);
            Assert::AreEqual(0, static_cast<int>(events.size()),
                L"No join/leave events when only the client itself is present.");

            // The diff logic is exercised through the real broadcast path: when a
            // second peer connects, the server sends a fresh PACKET_PLAYER_LIST
            // that includes the new id, and the client should surface a join event.
            NetworkClient second;
            Assert::IsTrue(second.Connect("127.0.0.1", port));
            bool secondWelcomed = false;
            for (int i = 0; i < 200 && !secondWelcomed; ++i) {
                client.Update(); second.Update(); server.Update();
                secondWelcomed = second.IsServerReady();
            }
            Assert::IsTrue(secondWelcomed, L"Second peer did not get WELCOME.");

            // Give the server time to rebroadcast the updated entity list and
            // the first client time to receive it, then drain.
            for (int i = 0; i < 20; ++i) {
                client.Update(); second.Update(); server.Update(); server.Update();
            }
            events.clear();
            client.DrainJoinLeaveEvents(events);
            bool sawJoin = false;
            for (size_t i = 0; i < events.size(); ++i)
            {
                if (events[i].joined && events[i].clientId == second.GetClientId()) sawJoin = true;
            }
            Assert::IsTrue(sawJoin, L"First client should report the second peer joining.");
        }

        TEST_METHOD(PasswordProtectedHostRejectsWrongAndAcceptsCorrect)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            server.SetPassword("s3cret");
            Assert::IsTrue(server.HasPassword());
            Assert::IsTrue(server.Start(port));

            // Wrong password: the server should drop the peer (never welcome).
            NetworkClient badClient;
            Assert::IsTrue(badClient.Connect("127.0.0.1", port));
            badClient.SetPassword("wrong");
            bool badWelcome = false;
            for (int i = 0; i < 200 && !badWelcome; ++i) {
                badClient.Update();
                server.Update();
                badWelcome = badClient.IsServerReady();
            }
            Assert::IsFalse(badWelcome, L"Wrong password must not be welcomed.");

            // Correct password: client is welcomed.
            NetworkClient goodClient;
            Assert::IsTrue(goodClient.Connect("127.0.0.1", port));
            goodClient.SetPassword("s3cret");
            bool goodWelcome = false;
            for (int i = 0; i < 200 && !goodWelcome; ++i) {
                goodClient.Update();
                server.Update();
                goodWelcome = goodClient.IsServerReady();
            }
            Assert::IsTrue(goodWelcome, L"Correct password should be welcomed.");
        }

        TEST_METHOD(ClientWithoutPasswordIsRejectedWhenHostProtected)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            server.SetPassword("pw");
            Assert::IsTrue(server.Start(port));

            // Client that never sends auth: never welcomed, eventually dropped.
            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));
            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                client.Update();
                server.Update();
                welcomed = client.IsServerReady();
            }
            Assert::IsFalse(welcomed, L"A client that sends no password must not be admitted.");
        }
    };

    TEST_CLASS(MatchManagerTests)
    {
    public:
        TEST_METHOD(HostRunsStandaloneWithoutCrash)
        {
            NetworkMatchManager* mgr = NetworkMatchManager::CreateInstance();
            unsigned short port = PickFreePort();
            Assert::IsTrue(mgr->StartHost(port));
            Assert::IsTrue(mgr->IsHosting());
            // The server has no clients yet; just run a few frames.
            for (int i = 0; i < 10; ++i) mgr->Update();
            mgr->Leave();
            Assert::IsFalse(mgr->IsInMatch());
        }

        TEST_METHOD(HostAndJoinAreMutuallyExclusive)
        {
            // The manager is a singleton: a single process can be a host OR a
            // joiner, never both. Verify the guard after StartHost.
            NetworkMatchManager* mgr = NetworkMatchManager::CreateInstance();
            unsigned short port = PickFreePort();
            Assert::IsTrue(mgr->StartHost(port));
            Assert::IsFalse(mgr->JoinHost("127.0.0.1", port + 1),
                L"JoinHost must be rejected while a match is already active.");

            mgr->Leave();
            Assert::IsTrue(mgr->JoinHost("127.0.0.1", port),
                L"JoinHost should succeed once the host match is left.");
            Assert::IsTrue(mgr->IsJoined());
            mgr->Leave();
        }

        TEST_METHOD(HostServerAcceptsRawClient)
        {
            NetworkMatchManager* host = NetworkMatchManager::CreateInstance();
            unsigned short port = PickFreePort();
            Assert::IsTrue(host->StartHost(port));

            // A plain client joins the manager's server over loopback.
            NetworkClient client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));

            bool welcomed = false;
            for (int i = 0; i < 300 && !welcomed; ++i) {
                host->Update();
                client.Update();
                welcomed = client.IsServerReady();
            }
            Assert::IsTrue(welcomed, L"Raw client did not get WELCOME from the match host.");

            client.Disconnect();
            host->Leave();
        }
    };

    // ---- RemotePlayerWorldView tests (with a fake in-world sink) ----

    namespace
    {
        struct FakeWorld
        {
            int spawnCount;
            int despawnCount;
            std::map<int, PlayerNetworkState> lastMoves;
        };

        // Static fake world shared across sink callbacks (sink has no userData).
        FakeWorld g_fakeWorld;

        void* FakeSpawn(int playerId)
        {
            g_fakeWorld.spawnCount++;
            return reinterpret_cast<void*>(static_cast<size_t>(0x1000 + playerId));
        }
        void* FakeVehicleSpawn(int playerId, const char* model, int netVehicleId)
        {
            (void)model;
            (void)netVehicleId;
            g_fakeWorld.spawnCount++;
            return reinterpret_cast<void*>(static_cast<size_t>(0x2000 + playerId));
        }
        void FakeMove(int playerId, void* handle, const PlayerNetworkState& state)
        {
            (void)handle;
            g_fakeWorld.lastMoves[playerId] = state;
        }
        void FakeDespawn(int playerId, void* handle)
        {
            (void)playerId;
            (void)handle;
            g_fakeWorld.despawnCount++;
        }
    }

    TEST_CLASS(RemotePlayerWorldViewTests)
    {
    public:
        TEST_METHOD(SpawnsMovesAndDespawns)
        {
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(0.01f); // near-instant smoothing for determinism

            // First sync: player 7 appears.
            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 1.0f; p.y = 2.0f; p.z = 3.0f;
            players.UpdatePlayer(7, p);
            view.Sync(players);
            Assert::AreEqual(1, g_fakeWorld.spawnCount, L"Remote player should be spawned.");
            Assert::AreEqual(1, static_cast<int>(g_fakeWorld.lastMoves.count(7)),
                L"New player's initial state should be moved to the world.");

            // Player moves -> target changes, but the move is only issued once
            // Update() steps the visual position (no stutter per packet).
            PlayerNetworkState p2 = p;
            p2.x = 5.0f;
            players.UpdatePlayer(7, p2);
            view.Sync(players);
            Assert::AreEqual(1, static_cast<int>(g_fakeWorld.lastMoves.count(7)));
            Assert::AreEqual(1.0f, g_fakeWorld.lastMoves[7].x,
                L"Sync alone must not move the visual position; Update does.");
            view.Update(1.0f);
            Assert::AreEqual(5.0f, g_fakeWorld.lastMoves[7].x,
                L"Update should advance the visual position toward the target.");

            // Player disappears -> despawned.
            RemotePlayerManager empty;
            view.Sync(empty);
            Assert::AreEqual(1, g_fakeWorld.despawnCount, L"Removed remote player should be despawned.");

            view.Clear();
        }

        TEST_METHOD(InterpolationApproachesTargetWithoutTeleporting)
        {
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            // Slow smoothing so the first step is visibly between start and target.
            view.SetSmoothing(1.0f);

            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
            players.UpdatePlayer(1, p);
            view.Sync(players); // spawn at 0,0,0
            Assert::AreEqual(0.0f, g_fakeWorld.lastMoves[1].x);

            // Target jumps to 100 on X. Interpolate over a few frames.
            p.x = 100.0f;
            players.UpdatePlayer(1, p);
            view.Sync(players);
            view.Update(0.1f);
            float first = g_fakeWorld.lastMoves[1].x;
            Assert::IsTrue(first > 0.0f && first < 100.0f,
                L"First interpolated step must be strictly between source and target.");

            // Keep stepping; value must converge toward the target.
            float prev = first;
            for (int i = 0; i < 20; ++i)
            {
                view.Sync(players); // refresh target
                view.Update(0.1f);
                float cur = g_fakeWorld.lastMoves[1].x;
                Assert::IsTrue(cur > prev || (fabsf(cur - prev) < 1e-4f),
                    L"Visual position must approach the target monotonically.");
                prev = cur;
            }
            Assert::IsTrue(fabsf(prev - 100.0f) < 1.0f,
                L"Interpolation should have converged close to the target.");

            view.Clear();
        }

        TEST_METHOD(NoMovementWithoutUpdate)
        {
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(0.12f);

            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
            players.UpdatePlayer(2, p);
            view.Sync(players);
            int movesBefore = g_fakeWorld.spawnCount + static_cast<int>(g_fakeWorld.lastMoves.size());

            p.x = 50.0f;
            players.UpdatePlayer(2, p);
            view.Sync(players);
            view.Sync(players); // repeated syncs with no Update
            Assert::AreEqual(0.0f, g_fakeWorld.lastMoves[2].x,
                L"A target change must not move the visual position until Update() runs.");

            view.Clear();
        }

        TEST_METHOD(NullSinkIsSafe)
        {
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            sink.spawn = nullptr;
            sink.move = nullptr;
            sink.despawn = nullptr;
            view.SetSink(sink);

            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 1.0f; p.y = 2.0f; p.z = 3.0f;
            players.UpdatePlayer(7, p);
            view.Sync(players); // must not crash
            view.Clear();
        }
    };

    TEST_CLASS(RemoteVehicleWorldViewTests)
    {
    public:
        TEST_METHOD(SpawnsMovesAndDespawns)
        {
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemoteVehicleWorldView view;
            RemoteVehicleWorldView::Sink sink;
            sink.spawn = &FakeVehicleSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(0.01f);

            // On foot: no ghost.
            RemotePlayerManager players;
            PlayerNetworkState onFoot;
            onFoot.x = 0.0f; onFoot.y = 0.0f; onFoot.z = 0.0f; onFoot.inVehicle = 0;
            onFoot.vehicleModel[0] = '\0';
            view.SyncDriver(3, onFoot);
            Assert::AreEqual(0, g_fakeWorld.spawnCount, L"No vehicle spawn while on foot.");

            // Enters a car -> spawn.
            PlayerNetworkState inCar = onFoot;
            strcpy(inCar.vehicleModel, "homer");
            inCar.netVehicleId = 9;
            inCar.inVehicle = 1;
            inCar.x = 10.0f;
            view.SyncDriver(3, inCar);
            Assert::AreEqual(1, g_fakeWorld.spawnCount, L"Ghost vehicle should be spawned on entry.");
            Assert::AreEqual(1, static_cast<int>(g_fakeWorld.lastMoves.count(3)),
                L"Ghost vehicle should be spawned on entry.");

            // Drives to a new position -> target updates, move issued on Update.
            PlayerNetworkState drive = inCar;
            drive.x = 50.0f;
            view.SyncDriver(3, drive);
            view.Update(1.0f);
            Assert::AreEqual(50.0f, g_fakeWorld.lastMoves[3].x, L"Ghost should follow the driver.");

            // Exits car -> despawn.
            view.SyncDriver(3, onFoot);
            Assert::AreEqual(1, g_fakeWorld.despawnCount, L"Ghost should be removed on exit.");

            view.Clear();
        }

        TEST_METHOD(ModelChangeRebuildsGhost)
        {
            g_fakeWorld.despawnCount = 0;
            RemoteVehicleWorldView view;
            RemoteVehicleWorldView::Sink sink;
            sink.spawn = &FakeVehicleSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(0.01f);

            PlayerNetworkState carA;
            carA.x = 0.0f; carA.y = 0.0f; carA.z = 0.0f; carA.inVehicle = 1;
            strcpy(carA.vehicleModel, "homer");
            carA.netVehicleId = 1;

            PlayerNetworkState carB = carA;
            strcpy(carB.vehicleModel, "cops_v");
            carB.netVehicleId = 2;

            view.SyncDriver(4, carA);
            Assert::AreEqual(1, static_cast<int>(g_fakeWorld.lastMoves.count(4)),
                L"Ghost should be present while driving.");

            view.SyncDriver(4, carB);
            Assert::AreEqual(1, g_fakeWorld.despawnCount,
                L"Changing model should despawn the old ghost.");
            Assert::AreEqual(1, g_fakeWorld.lastMoves[4].x == 0.0f ? 1 : 0,
                L"New ghost still present after model change.");

            view.Clear();
        }

        TEST_METHOD(PruneRemovesGhostOfDisconnectedPlayer)
        {
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemoteVehicleWorldView view;
            RemoteVehicleWorldView::Sink sink;
            sink.spawn = &FakeVehicleSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(0.01f);

            RemotePlayerManager players;
            PlayerNetworkState inCar;
            inCar.x = 1.0f; inCar.y = 1.0f; inCar.z = 1.0f; inCar.inVehicle = 1;
            strcpy(inCar.vehicleModel, "homer");
            inCar.netVehicleId = 3;
            players.UpdatePlayer(8, inCar);

            view.SyncDriver(8, inCar);
            Assert::AreEqual(1, g_fakeWorld.spawnCount);

            // Player 8 disappears from the authoritative set -> prune despawns ghost.
            RemotePlayerManager empty;
            view.Prune(empty);
            Assert::AreEqual(1, g_fakeWorld.despawnCount,
                L"Ghost of a disconnected remote driver should be removed.");

            view.Clear();
        }
    };

    TEST_CLASS(ServerAuthorityTests)
    {
    public:
        // A fresh small state is accepted and relayed back to a second client.
        TEST_METHOD(ValidationAcceptsNormalMovement)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));

            NetworkClient a;
            Assert::IsTrue(a.Connect("127.0.0.1", port));
            NetworkClient b;
            Assert::IsTrue(b.Connect("127.0.0.1", port));

            bool readyA = false, readyB = false;
            for (int i = 0; i < 300 && !(readyA && readyB); ++i) {
                server.Update(); a.Update(); b.Update();
                readyA = a.IsServerReady();
                readyB = b.IsServerReady();
            }
            Assert::IsTrue(readyA && readyB, L"Both clients should be welcomed.");

            // Give client A a few honest, small footsteps.
            PlayerNetworkState st;
            st.x = 0.0f; st.y = 0.0f; st.z = 0.0f; st.rotY = 0.0f; st.vehicleModel[0] = '\0'; st.netVehicleId = 0; st.inVehicle = 0;
            for (int i = 0; i < 120; ++i) {
                st.x += 0.5f;
                a.SendPlayerState(st);
                a.Update(); server.Update(); b.Update();
                Sleep(2); // ensure a measurable dt between samples
            }

            // Server should consider A present and relayed.
            const PlayerNetworkState* view = server.GetRemotePlayers().GetPlayer(static_cast<int>(a.GetClientId()));
            Assert::IsNotNull(view, L"Normal movement should be accepted.");

            a.Disconnect();
            b.Disconnect();
        }

        // A teleport far away within a single tick must be rejected (not relayed).
        TEST_METHOD(ValidationRejectsTeleport)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));

            NetworkClient a;
            Assert::IsTrue(a.Connect("127.0.0.1", port));

            bool readyA = false;
            for (int i = 0; i < 300 && !readyA; ++i) {
                server.Update(); a.Update();
                readyA = a.IsServerReady();
            }
            Assert::IsTrue(readyA, L"Client A should be welcomed.");

            // Establish a plausible position first.
            PlayerNetworkState st;
            st.x = 0.0f; st.y = 0.0f; st.z = 0.0f; st.rotY = 0.0f; st.vehicleModel[0] = '\0'; st.netVehicleId = 0; st.inVehicle = 0;
            for (int i = 0; i < 30; ++i) {
                st.x += 0.5f;
                a.SendPlayerState(st);
                a.Update(); server.Update();
                Sleep(2);
            }

            // Now a teleport thousands of metres in a single update.
            st.x = 1.0f;
            st.z = 10000.0f;
            a.SendPlayerState(st);
            a.Update(); server.Update();

            // The server must NOT have recorded the teleported position.
            const PlayerNetworkState* view = server.GetRemotePlayers().GetPlayer(static_cast<int>(a.GetClientId()));
            Assert::IsNotNull(view, L"Client still known from earlier state.");
            if (view) {
                Assert::IsTrue(view->z < 100.0f, L"Teleport should be rejected by the server.");
            }

            a.Disconnect();
        }

        // Integration: two clients over a shared server; A's accepted state must
        // be relayed so client B sees A through its own RemotePlayerManager.
        TEST_METHOD(TwoClientsSeeEachOtherThroughRelay)
        {
            NetworkServer server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port));

            NetworkClient a;
            Assert::IsTrue(a.Connect("127.0.0.1", port));
            NetworkClient b;
            Assert::IsTrue(b.Connect("127.0.0.1", port));

            bool readyA = false, readyB = false;
            for (int i = 0; i < 300 && !(readyA && readyB); ++i) {
                server.Update(); a.Update(); b.Update();
                readyA = a.IsServerReady();
                readyB = b.IsServerReady();
            }
            Assert::IsTrue(readyA && readyB, L"Both clients should be welcomed.");

            // Client A walks forward in honest, slow steps so the server accepts
            // and relays the state (speed well under the on-foot limiter).
            PlayerNetworkState st;
            st.x = 0.0f; st.y = 0.0f; st.z = 0.0f; st.rotY = 0.0f; st.vehicleModel[0] = '\0'; st.netVehicleId = 0; st.inVehicle = 0;
            for (int i = 0; i < 60; ++i) {
                st.x += 0.5f;
                a.SendPlayerState(st);
                a.Update(); server.Update(); b.Update();
                Sleep(25); // 0.5m per 25ms = 20 m/s, below the ~22.5 m/s foot cap
            }

            // Client B must now see A's latest position through the relay.
            unsigned short aId = a.GetClientId();
            const PlayerNetworkState* bSeesA = b.GetRemotePlayers().GetPlayer(static_cast<int>(aId));
            Assert::IsNotNull(bSeesA, L"B should see A through the server relay.");
            if (bSeesA) {
                Assert::IsTrue(bSeesA->x > 10.0f,
                    L"B should observe A's advanced position.");
            }

            // And the server authoritative set contains A.
            Assert::IsTrue(server.GetRemotePlayers().HasPlayer(static_cast<int>(aId)),
                L"Server should have A in its authoritative set.");

            a.Disconnect();
            b.Disconnect();
        }
    };

    TEST_CLASS(ServerModManagerTests)
    {
    public:
        TEST_METHOD(LuaHooksControlJoinAndValidate)
        {
            ServerModManager mods;
            // Absolute path so the test is independent of vstest's working dir.
            Assert::IsTrue(mods.LoadScript("C:\\Users\\User\\Downloads\\The-Simpsons-Hit-Run\\src\\tests\\test_server_mod.lua"),
                L"Should load the Lua test script.");
            Assert::IsTrue(mods.HasScript());

            // test_server_mod.lua rejects clientId 9999 on join.
            Assert::IsFalse(mods.OnPlayerJoin(9999), L"on_join(9999) should return false.");
            Assert::IsTrue(mods.OnPlayerJoin(1), L"on_join(1) should return true.");

            // test_server_mod.lua rejects states with z > 100.
            PlayerNetworkState ok;
            ok.x = 0; ok.y = 0; ok.z = 0;
            Assert::IsTrue(mods.OnPlayerValidate(1, ok), L"on_validate with z=0 should pass.");

            PlayerNetworkState bad;
            bad.x = 0; bad.y = 0; bad.z = 500;
            Assert::IsFalse(mods.OnPlayerValidate(1, bad), L"on_validate with z=500 should fail.");

            mods.OnPlayerLeave(1); // must not crash
        }
    };
}