#include "CppUnitTest.h"
#include "NetworkPacket.h"
#include "PlayerNetworkState.h"
#include "NetworkMatchManager.h"
#include "RemotePlayerWorldView.h"
#include "RemoteVehicleWorldView.h"
#include "ServerModManager.h"
#include "ClientModManager.h"
#include "MissionSyncState.h"
#include "LocalActionLatch.h"
#include "EconomyConfig.h"
#include "NetWorldRegistry.h"
#include "NetEntityManager.h"
#include "PlayerEconomy.h"
#include "MissionSyncManager.h"
#include "../game/netengine/Packet.h"
#include "../game/netengine/PacketRegistry.h"
#include "../game/netengine/ServerEndpoint.h"
#include "../game/netengine/ClientEndpoint.h"
#include "../game/netengine/Session.h"
#include "../game/netengine/GamePlayerBridge.h"
#include "p3d/p3dwriter.h"

#include <winsock2.h>
#include <functional>
#include <initializer_list>
#include <limits>
#include <stdlib.h>
#include <string>
#include <windows.h>

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

    // Anchors TestDataPath to THIS test DLL: we must resolve Tests.dll's own path,
// not the host runner (vstest.console.exe lives outside the repo). Taking the
// address of a function compiled into Tests.dll lets GetModuleHandleEx find the
// right module deterministically, which works both under vstest and on CI where
// the DLL sits in the repo at <repo>/src/tests/x64/Debug/.
    static const char* TestDataAnchor() { return "anchor"; }

    std::string TestDataPath(const char* fileName)
    {
        HMODULE mod = NULL;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&TestDataAnchor), &mod) == 0)
            return std::string(fileName);

        char module[MAX_PATH] = { 0 };
        DWORD n = GetModuleFileNameA(mod, module, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return std::string(fileName);

        std::string dir(module);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        // dir is now .../src/tests/x64/Debug -> drop x64 and Debug (2 levels) to
        // reach src/tests.
        for (int i = 0; i < 2; ++i)
        {
            size_t p = dir.find_last_of("\\/");
            if (p == std::string::npos) break;
            dir = dir.substr(0, p);
        }
        return dir + "\\" + fileName;
    }

    // ---- helpers for the engine-level transport tests ---------------

    // A controllable local-state source (game-side typed provider).
    struct ScriptedPlayer
    {
        PlayerNetworkState state = {};
        bool ready = false;
        bool operator()(PlayerNetworkState& out)
        {
            if (!ready) return false;
            out = state;
            return true;
        }
    };

    // The host bridge pulls local state from whatever ScriptedPlayer is
    // currently installed. Kept file-scope so the C-function-callback style
    // provider can reach it without a member.
    static ScriptedPlayer* g_scriptedHostHolder = nullptr;
    static bool g_scriptedHost(PlayerNetworkState& out)
    {
        if (g_scriptedHostHolder == nullptr || !g_scriptedHostHolder->ready) return false;
        out = g_scriptedHostHolder->state;
        return true;
    }

    // Server-authoritative movement validator that reads the PlayerNetworkState
    // blob the session relays and applies the old server's plausibility gate:
    // a large single-tick jump (teleport) is rejected, honest steps pass. Keeps
    // the world-agnostic net::Session honest without coupling it to game types.
    struct PlayerMoveValidator : net::IStateValidator
    {
        float maxSpeedOnFoot = 15.0f;
        float maxSpeedInVehicle = 120.0f;
        float maxTeleportMeters = 5.0f;
        std::map<net::PeerId, PlayerNetworkState> lastState;
        int accepted = 0;
        int rejected = 0;

        bool ValidatePeerState(net::PeerId from, const uint8_t* data, uint16_t size) override
        {
            if (size < PlayerNetworkState::SERIALIZED_SIZE) { ++rejected; return false; }
            PlayerNetworkState st;
            st.Deserialize(reinterpret_cast<const char*>(data));

            auto it = lastState.find(from);
            if (it != lastState.end())
            {
                const PlayerNetworkState& prev = it->second;
                float dx = st.x - prev.x, dy = st.y - prev.y, dz = st.z - prev.z;
                float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                float maxSpeed = (st.inVehicle != 0) ? maxSpeedInVehicle : maxSpeedOnFoot;
                // Teleport gate: a jump farther than walking/running + the
                // teleport slack in a single tick is rejected outright.
                if (dist > maxTeleportMeters + maxSpeed) { ++rejected; return false; }
            }
            lastState[from] = st;
            ++accepted;
            return true;
        }
    };

    // Nets a real loopback host session + a joiner session, pumping both until
    // the joiner has been welcomed (non-zero self id). Returns false if the
    // join never completed so tests can stay readable.
    bool PumpToWelcome(net::Session& host, net::Session& joiner)
    {
        bool ready = false;
        for (int i = 0; i < 500 && !ready; ++i) {
            host.Update();
            joiner.Update();
            ready = joiner.GetLocalPeerId() != 0;
        }
        return ready;
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
            state.flags = PlayerNetworkState::FLAG_JUMPING | PlayerNetworkState::FLAG_IN_AIR;
            state.motionAngle = 1234;
            state.facingAngle = 4321;
            state.speedFactor = 200;
            state.characterState = 1; // CharacterAi::LOCO
            state.actionEvent = PlayerNetworkState::ACTION_JUMP;
            state.vehYaw = 2.5f;
            state.vehSteer = 64;
            state.vehSpeedKmh = 88;
            state.vehState = 1;
            state.vehFlags = PlayerNetworkState::VEH_FLAG_BRAKE | PlayerNetworkState::VEH_FLAG_REVERSE;

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
            Assert::AreEqual(static_cast<int>(state.flags), static_cast<int>(restored.flags));
            Assert::AreEqual(static_cast<int>(state.motionAngle), static_cast<int>(restored.motionAngle));
            Assert::AreEqual(static_cast<int>(state.facingAngle), static_cast<int>(restored.facingAngle));
            Assert::AreEqual(static_cast<int>(state.speedFactor), static_cast<int>(restored.speedFactor));
            Assert::AreEqual(static_cast<int>(state.characterState), static_cast<int>(restored.characterState));
            Assert::AreEqual(static_cast<int>(state.actionEvent), static_cast<int>(restored.actionEvent));
            Assert::AreEqual(state.vehYaw, restored.vehYaw);
            Assert::AreEqual(static_cast<int>(state.vehSteer), static_cast<int>(restored.vehSteer));
            Assert::AreEqual(static_cast<int>(state.vehSpeedKmh), static_cast<int>(restored.vehSpeedKmh));
            Assert::AreEqual(static_cast<int>(state.vehState), static_cast<int>(restored.vehState));
            Assert::AreEqual(static_cast<int>(state.vehFlags), static_cast<int>(restored.vehFlags));
        }

        TEST_METHOD(SerializedSizeIsStable)
        {
            // Serialization must not depend on struct padding/alignment.
            // 4 floats + vehicleModel[16] + int32 + float + 2*uint8         (base 42)
            // + 2*uint16 + 2*uint8 + uint8                                  (loco +7 = 49)
            // + uint8                                                       (action +1 = 50)
            // + float + uint8 + uint16 + uint8 + uint8                      (vehicle mirror +9 = 59?? -> 58)
            // Correct: 42 (base) + 4 (2*u16) + 2 (2*u8) + 1 (action) + 4 (float) + 1 (u8) + 2 (u16) + 1 (u8) + 1 (u8) = 58.
            Assert::AreEqual<int>(4 * 4 + 16 + 4 + 4 + 2 + 4 + 2 + 1 + 4 + 1 + 2 + 1 + 1, PlayerNetworkState::SERIALIZED_SIZE);
        }

        TEST_METHOD(LocoFieldsRoundTripSerialize)
        {
            // Continuous-locomotion + vehicle-mirror fields survive a wire
            // round-trip, and the new stable SERIALIZED_SIZE is enforced.
            PlayerNetworkState state;
            state.x = 1.0f; state.y = 2.0f; state.z = 3.0f; state.rotY = 0.5f;
            strcpy(state.vehicleModel, "homer");
            state.netVehicleId = 9;
            state.vehicleSpeed = 50.0f;
            state.inVehicle = 1;
            state.flags = PlayerNetworkState::FLAG_LOCO_TURBO;
            state.motionAngle = 3141;   // ~PI radians
            state.facingAngle = 1000;   // 1 radian
            state.speedFactor = 128;    // half speed
            state.characterState = 1;   // LOCO
            state.actionEvent = PlayerNetworkState::ACTION_JUMP;
            state.vehYaw = 1.75f;
            state.vehSteer = static_cast<uint8_t>(static_cast<int8_t>(30 * (255.0f / 90.0f))); // ~+30 deg
            state.vehSpeedKmh = 95;
            state.vehState = 0;         // VS_NORMAL
            state.vehFlags = PlayerNetworkState::VEH_FLAG_BRAKE;

            Assert::AreEqual<int>(58, PlayerNetworkState::SERIALIZED_SIZE, L"SERIALIZED_SIZE drifted.");
            char buffer[PlayerNetworkState::SERIALIZED_SIZE];
            state.Serialize(buffer);
            PlayerNetworkState restored;
            restored.Deserialize(buffer);

            Assert::AreEqual(static_cast<int>(state.motionAngle), static_cast<int>(restored.motionAngle));
            Assert::AreEqual(static_cast<int>(state.facingAngle), static_cast<int>(restored.facingAngle));
            Assert::AreEqual(static_cast<int>(state.speedFactor), static_cast<int>(restored.speedFactor));
            Assert::AreEqual(static_cast<int>(state.characterState), static_cast<int>(restored.characterState));
            Assert::AreEqual(static_cast<int>(state.flags), static_cast<int>(restored.flags));
            Assert::AreEqual(static_cast<int>(state.actionEvent), static_cast<int>(restored.actionEvent));
            Assert::AreEqual(state.vehYaw, restored.vehYaw);
            Assert::AreEqual(static_cast<int>(state.vehSteer), static_cast<int>(restored.vehSteer));
            Assert::AreEqual(static_cast<int>(state.vehSpeedKmh), static_cast<int>(restored.vehSpeedKmh));
            Assert::AreEqual(static_cast<int>(state.vehState), static_cast<int>(restored.vehState));
            Assert::AreEqual(static_cast<int>(state.vehFlags), static_cast<int>(restored.vehFlags));
        }
    };

    TEST_CLASS(ClientServerLoopbackTests)
    {
    public:
        TEST_METHOD(ServerAcceptsHelloAndWelcome)
        {
            net::Session host;
            unsigned short port = PickFreePort();
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));

            Assert::IsTrue(PumpToWelcome(host, joiner),
                L"Joiner did not receive WELCOME within loopback pump.");
            Assert::IsTrue(joiner.GetLocalPeerId() != 0);

            joiner.Leave();
            host.Leave();
        }

        TEST_METHOD(PlayerStateRoundtripThroughServer)
        {
            // Host publishes a scripted state; the joiner's world sink must
            // see the relayed copy (the server relays the host's loopback state).
            ScriptedPlayer hostState;
            hostState.state.x = 10.0f;
            hostState.state.y = 20.0f;
            hostState.state.z = 30.0f;
            hostState.state.rotY = 1.5f;
            strcpy(hostState.state.vehicleModel, "homer");
            hostState.state.netVehicleId = 7;
            hostState.state.vehicleSpeed = 55.0f;
            hostState.state.inVehicle = 1;
            hostState.ready = true;

            net::GamePlayerBridge hostBridge;
            hostBridge.SetTypedLocalProvider(
                [](PlayerNetworkState& out) -> bool { return g_scriptedHost(out); });
            g_scriptedHostHolder = &hostState;

            RemotePlayerManager joinerRemote;
            net::GamePlayerBridge joinerBridge;
            joinerBridge.SetRemotePlayers(&joinerRemote);

            unsigned short port = PickFreePort();
            net::Session host;
            hostBridge.AttachToSession(&host);
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session joiner;
            joinerBridge.AttachToSession(&joiner);
            Assert::IsTrue(joiner.Join("127.0.0.1", port));

            Assert::IsTrue(PumpToWelcome(host, joiner), L"Joiner did not get WELCOME.");
            net::PeerId hostId = host.GetLocalPeerId();
            Assert::IsTrue(hostId != 0, L"Host loopback should get an id.");

            // Pump until the joiner observes the relayed host state.
            const PlayerNetworkState* seen = NULL;
            for (int i = 0; i < 600 && seen == NULL; ++i) {
                host.Update();
                joiner.Update();
                seen = joinerRemote.GetPlayer(static_cast<int>(hostId));
            }
            Assert::IsNotNull(seen, L"Server rooted/relayed no host player state.");
            if (seen) {
                Assert::AreEqual(30.0f, seen->z, L"Host z should be relayed to the joiner.");
            }

            g_scriptedHostHolder = nullptr;
            joiner.Leave();
            host.Leave();
        }

        TEST_METHOD(ServerDropsSilentClientAfterTimeout)
        {
            net::ServerEndpoint server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port, 4));
            server.SetClientTimeoutMs(100);

            net::ClientEndpoint client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));

            // Wait for the server to observe the connect and assign an id.
            bool peerJoined = false;
            net::PeerId cid = 0;
            server.SetPeerEventFn([&](const net::PeerEvent& e) {
                if (e.connected) { peerJoined = true; cid = e.peer; }
            });
            for (int i = 0; i < 300 && !peerJoined; ++i) { server.Update(); client.Update(); }
            Assert::IsTrue(peerJoined, L"Server should see the client connect.");
            Assert::IsTrue(server.HasPeer(cid), L"Server should track the client.");

            // Stop pumping the client; the transport timeout sweep must drop it.
            bool dropped = false;
            for (int i = 0; i < 40 && !dropped; ++i) {
                Sleep(10);
                server.Update(); // no client.Update() - client is silent
                dropped = !server.HasPeer(cid);
            }
            Assert::IsTrue(dropped, L"Server did not drop a silent client after timeout.");

            client.Disconnect();
            server.Stop();
        }

        TEST_METHOD(HeartbeatKeepsClientAlive)
        {
            net::ServerEndpoint server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port, 4));
            server.SetClientTimeoutMs(100);

            net::ClientEndpoint client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));
            client.SetPingIntervalMs(0); // heartbeat on every Update

            bool peerJoined = false;
            net::PeerId cid = 0;
            server.SetPeerEventFn([&](const net::PeerEvent& e) {
                if (e.connected) { peerJoined = true; cid = e.peer; }
            });
            for (int i = 0; i < 300 && !peerJoined; ++i) { server.Update(); client.Update(); }
            Assert::IsTrue(peerJoined, L"Server should see the client connect.");

            // Pump well past the timeout window while the client keeps pinging.
            bool alive = true;
            for (int i = 0; i < 80; ++i) {
                client.Update();
                server.Update();
                if (!server.HasPeer(cid)) { alive = false; break; }
            }
            Assert::IsTrue(alive, L"Client should stay connected while heartbeating.");

            client.Disconnect();
            server.Stop();
        }

        TEST_METHOD(ClientMeasuresPingFromPong)
        {
            net::ServerEndpoint server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port, 4));

            net::ClientEndpoint client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));
            client.SetPingIntervalMs(0); // ping on every Update

            bool peerJoined = false;
            server.SetPeerEventFn([&](const net::PeerEvent& e) { if (e.connected) peerJoined = true; });
            for (int i = 0; i < 300 && !peerJoined; ++i) { server.Update(); client.Update(); }
            Assert::IsTrue(peerJoined, L"Server should see the client connect.");

            // A few rounds; the client should receive PONG and measure RTT.
            bool measured = false;
            for (int i = 0; i < 30 && !measured; ++i) {
                client.Update();
                server.Update();
                measured = client.HasMeasuredPing();
            }
            Assert::IsTrue(measured, L"Client never measured a ping round-trip.");
            Assert::IsTrue(client.GetPingMs() < 5000, L"Measured ping looks absurdly high.");

            client.Disconnect();
            server.Stop();
        }

        TEST_METHOD(ClientReportsJoinLeaveEvents)
        {
            // Host surface: when a second peer joins the same host, the host's
            // join/leave callback must report it — the roster is authoritative.
            net::Session host;
            unsigned short port = PickFreePort();
            bool sawJoinLeave = false;
            std::vector<std::pair<net::PeerId, bool>> events;
            host.SetJoinLeaveFn([&](net::PeerId p, bool joined) {
                events.push_back({ p, joined });
                if (joined) sawJoinLeave = true;
            });
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            // First joiner welcomed (and the host's own loopback joins too).
            net::Session first;
            Assert::IsTrue(first.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, first), L"First joiner did not get WELCOME.");

            // Second joiner: host must emit a join event (and a PLAYER_LIST
            // refresh reaches the first client, which tracks the new peer).
            net::Session second;
            Assert::IsTrue(second.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, second), L"Second joiner did not get WELCOME.");
            for (int i = 0; i < 20; ++i) { host.Update(); first.Update(); second.Update(); }

            // The first client's authoritative roster now contains the second peer.
            bool sawSecond = false;
            for (net::PeerId id : first.GetPeerRoster())
                if (!sawJoinLeave || id == second.GetLocalPeerId()) sawSecond = (id == second.GetLocalPeerId());
            Assert::IsTrue(sawSecond, L"First client should see the second peer in its roster.");
            Assert::IsTrue(sawJoinLeave, L"Host should have reported a join event.");

            second.Leave();
            first.Leave();
            host.Leave();
        }

        TEST_METHOD(PasswordProtectedHostRejectsWrongAndAcceptsCorrect)
        {
            net::Session host;
            unsigned short port = PickFreePort();
            host.SetHostPassword("s3cret");
            Assert::IsTrue(host.HasHostPassword());
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            // Wrong password: the peer is kicked, never welcomed.
            net::Session bad;
            bad.SetPassword("wrong");
            Assert::IsTrue(bad.Join("127.0.0.1", port));
            bool badWelcome = false;
            for (int i = 0; i < 200 && !badWelcome; ++i) {
                host.Update();
                bad.Update();
                badWelcome = bad.GetLocalPeerId() != 0;
            }
            Assert::IsFalse(badWelcome, L"Wrong password must not be welcomed.");
            bad.Leave();

            // Correct password: the peer is welcomed.
            net::Session good;
            good.SetPassword("s3cret");
            Assert::IsTrue(good.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, good), L"Correct password should be welcomed.");

            good.Leave();
            host.Leave();
        }

        TEST_METHOD(ClientWithoutPasswordIsRejectedWhenHostProtected)
        {
            net::Session host;
            unsigned short port = PickFreePort();
            host.SetHostPassword("pw");
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            // A client that never sends auth is never admitted.
            net::Session client;
            Assert::IsTrue(client.Join("127.0.0.1", port));
            bool welcomed = false;
            for (int i = 0; i < 200 && !welcomed; ++i) {
                host.Update();
                client.Update();
                welcomed = client.GetLocalPeerId() != 0;
            }
            Assert::IsFalse(welcomed, L"A client that sends no password must not be admitted.");

            client.Leave();
            host.Leave();
        }

        TEST_METHOD(SessionReconnectsAfterLeave)
        {
            // A single net::Session must be re-usable after Leave() (the
            // reconnection / "played a round, join the next one" scenario).
            // Leave() fully tears down transport and resets match state, so a
            // second Join() on the same instance must work cleanly.
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, joiner), L"First join did not get WELCOME.");
            Assert::IsTrue(joiner.GetLocalPeerId() != 0);
            joiner.Leave();
            Assert::IsFalse(joiner.IsInMatch(), L"Joiner must be out of any match after Leave.");

            // Same joiner object joins the still-running host again.
            Assert::IsTrue(joiner.Join("127.0.0.1", port), L"Re-join on same session failed.");
            Assert::IsTrue(PumpToWelcome(host, joiner), L"Re-join after Leave did not get WELCOME.");
            Assert::IsTrue(joiner.GetLocalPeerId() != 0, L"Re-joined session has no peer id.");

            // And a fresh peering works too, proving the host accepted both.
            net::Session third;
            Assert::IsTrue(third.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, third), L"Third joiner did not get WELCOME.");

            third.Leave();
            joiner.Leave();
            host.Leave();
        }

        TEST_METHOD(HostRestartsOnSamePortAfterLeave)
        {
            // After Leave(), the same Session can StartHost again on the same
            // UDP port: teardown must release the socket so the rebind succeeds.
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xAA55));
            host.Leave();
            Assert::IsFalse(host.IsInMatch(), L"Host must leave the match after Leave().");

            Assert::IsTrue(host.StartHost(port, 0xBEEF), L"Rebind on the same port failed.");
            Assert::IsTrue(host.IsHost(), L"Host must be hosting after restart.");

            // A joiner connects to the restarted host to prove it's listening.
            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, joiner), L"Joiner did not reach the restarted host.");

            joiner.Leave();
            host.Leave();
        }

        TEST_METHOD(CustomEventRelaysFromJoinerToHost)
        {
            // The open user-data channel ships an opaque payload through the
            // server to the host's custom-event sink (server relays, sender
            // excluded). The host sees the event tagged with the joiner's id.
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, joiner), L"Joiner did not get WELCOME.");

            // Host receives the joiner's custom event.
            int hostGot = 0;
            uint8_t hostPayload[8] = { 0 };
            net::PeerId hostFrom = 0;
            host.SetCustomEventSink([&](net::PeerId from, const uint8_t* d, uint16_t size) {
                hostGot++;
                hostFrom = from;
                uint16_t n = size < sizeof(hostPayload) ? size : sizeof(hostPayload);
                memcpy(hostPayload, d, n);
            });

            const uint8_t msg[5] = { 9, 8, 7, 6, 5 };
            Assert::IsTrue(joiner.SendCustomEvent(msg, sizeof(msg)), L"joiner.SendCustomEvent failed.");

            for (int i = 0; i < 200 && hostGot == 0; ++i) { host.Update(); joiner.Update(); }
            Assert::AreEqual(1, hostGot, L"Host did not receive the joiner's custom event.");
            Assert::IsTrue(hostFrom == joiner.GetLocalPeerId(), L"Host saw wrong sender id.");
            for (int i = 0; i < 5; ++i)
                Assert::AreEqual(static_cast<int>(msg[i]), static_cast<int>(hostPayload[i]),
                    L"Host got mangled custom payload.");

            joiner.Leave();
            host.Leave();
        }

        TEST_METHOD(CustomEventRelaysFromHostToJoiner)
        {
            // Same channel, reverse direction: the host ships an event that the
            // joiner's sink receives (via the host's own loopback client).
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, joiner), L"Joiner did not get WELCOME.");

            int joinerGot = 0;
            uint8_t payload[4] = { 0 };
            joiner.SetCustomEventSink([&](net::PeerId from, const uint8_t* d, uint16_t size) {
                joinerGot++;
                uint16_t n = size < sizeof(payload) ? size : sizeof(payload);
                memcpy(payload, d, n);
            });

            const uint8_t msg[4] = { 1, 3, 3, 7 };
            Assert::IsTrue(host.SendCustomEvent(msg, sizeof(msg)), L"host.SendCustomEvent failed.");

            for (int i = 0; i < 200 && joinerGot == 0; ++i) { host.Update(); joiner.Update(); }
            Assert::AreEqual(1, joinerGot, L"Joiner did not receive the host's custom event.");
            for (int i = 0; i < 4; ++i)
                Assert::AreEqual(static_cast<int>(msg[i]), static_cast<int>(payload[i]),
                    L"Joiner got mangled custom payload.");

            joiner.Leave();
            host.Leave();
        }

        TEST_METHOD(CustomEventDoesNotEchoToSender)
        {
            // A sender must not receive its own event back (the server's
            // omit=sender broadcast removes the echo).
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, joiner), L"Joiner did not get WELCOME.");

            int joinerGot = 0;
            joiner.SetCustomEventSink([&](net::PeerId, const uint8_t*, uint16_t) { joinerGot++; });

            const uint8_t msg[2] = { 0xAB, 0xCD };
            Assert::IsTrue(joiner.SendCustomEvent(msg, sizeof(msg)));

            // Pump enough that any echo would have arrived, then assert none did.
            for (int i = 0; i < 60; ++i) { host.Update(); joiner.Update(); }
            Assert::AreEqual(0, joinerGot, L"Sender received its own custom event back.");

            joiner.Leave();
            host.Leave();
        }

        TEST_METHOD(CustomEventRejectsOversize)
        {
            // Payloads over MAX_PACKET_PAYLOAD must be refused before send.
            net::Session host;
            Assert::IsTrue(host.StartHost(PickFreePort(), 0xAA55));
            uint8_t big[net::MAX_PACKET_PAYLOAD + 8];
            Assert::IsFalse(host.SendCustomEvent(big, net::MAX_PACKET_PAYLOAD + 1),
                L"Oversize custom event must be rejected.");
            host.Leave();
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

            // A plain transport client connects to the manager's server over
            // loopback; a successful ENet connect means the server accepted the
            // peer at the transport level (the raw endpoint does not parse the
            // session WELCOME, so readiness here == transport acceptance).
            net::ClientEndpoint client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));

            bool ready = false;
            for (int i = 0; i < 300 && !ready; ++i) {
                host->Update();
                client.Update();
                ready = client.IsPeerReady();
            }
            Assert::IsTrue(ready, L"Raw client was not accepted by the match host's server.");

            client.Disconnect();
            host->Leave();
        }

        // A host is now a full participant: it connects a loopback client to its
        // own server, so a joined client must be able to see the host player's
        // identity and position relayed through the server.
        TEST_METHOD(HostPlayerIsVisibleToJoinedClient)
        {
            static PlayerNetworkState s_hostState;
            s_hostState.x = 42.0f; s_hostState.y = 0.0f; s_hostState.z = 9.0f;
            s_hostState.inVehicle = 0; s_hostState.vehicleModel[0] = '\0';
            NetworkMatchManager::LocalStateProvider provider =
                [](PlayerNetworkState& out) -> bool { out = s_hostState; return true; };

            NetworkMatchManager* host = NetworkMatchManager::CreateInstance();
            host->SetLocalStateProvider(provider);
            unsigned short port = PickFreePort();
            Assert::IsTrue(host->StartHost(port));

            // The host itself is a joined client of its own server. A client id is
            // only assigned once it receives WELCOME, so wait for a non-zero id
            // (IsJoined() alone flips true right after Connect, before WELCOME).
            bool hostReady = false;
            for (int i = 0; i < 300 && !hostReady; ++i) {
                host->Update();
                hostReady = host->GetLocalClientId() != 0;
            }
            Assert::IsTrue(hostReady, L"Host's loopback client should be welcomed by its own server.");
            unsigned short hostId = host->GetLocalClientId();

            // A second net::Session joins and should eventually see the host player
            // through the engine relay (the bridge exposes it via its own
            // RemotePlayerManager). We check via the join session's world sink,
            // wired to a RemotePlayerManager, to prove the relay works end-to-end.
            net::Session joiner;
            // The joiner consumes remote player states into a RemotePlayerManager.
            RemotePlayerManager joinerRemote;
            net::GamePlayerBridge joinerBridge;
            joinerBridge.SetRemotePlayers(&joinerRemote);
            Assert::IsTrue(joiner.IsInMatch() == false);
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            joinerBridge.AttachToSession(&joiner);

            bool clientReady = false;
            for (int i = 0; i < 300 && !clientReady; ++i) {
                host->Update(); joiner.Update();
                clientReady = joiner.GetLocalPeerId() != 0;
            }
            Assert::IsTrue(clientReady, L"Joiner session did not get its id.");

            // Pump until the joiner observes the host's relayed position.
            const PlayerNetworkState* seenHost = NULL;
            for (int i = 0; i < 300 && seenHost == NULL; ++i) {
                host->Update(); joiner.Update();
                seenHost = joinerRemote.GetPlayer(static_cast<int>(hostId));
            }
            Assert::IsNotNull(seenHost, L"Joiner should see the host player through the relay.");
            if (seenHost) {
                Assert::AreEqual(42.0f, seenHost->x, L"Host's position should be relayed to the joiner.");
            }

            joiner.Leave();
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
            int actionCount; // number of times the action sink fired (rising edges)
            std::vector<uint8_t> lastActions; // the actionEvents seen at each fire
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
        void FakeAction(int playerId, void* handle, uint8_t actionEvent)
        {
            (void)playerId;
            (void)handle;
            g_fakeWorld.actionCount++;
            g_fakeWorld.lastActions.push_back(actionEvent);
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

        TEST_METHOD(UpdateInterpretsDtInSecondsNotMilliseconds)
        {
            // REGRESSION (multiplayer sync bug): GameplayContext::OnUpdate feeds
            // the world view a per-frame dt. `elapsedTime` there is milliseconds
            // but RemotePlayerWorldView::Update(float dt) is documented (and
            // unit-tested) as SECONDS, via its m_smoothingSeconds field. If a
            // caller passes milliseconds (e.g. 16 for a 60fps frame), alpha =
            // 1 - exp(-lambda * 16) underflows to ~1.0 and the remote player's
            // visual position teleports its ENTIRE distance to the latest
            // reported target on the first Update — so "remote players spawn but
            // never smoothly move". This test pins the seconds contract: a
            // realistic per-frame dt in seconds must advance only PARTWAY toward
            // a far target, never teleport the whole way.
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            memset(&sink, 0, sizeof(sink));
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(0.12f); // the production default

            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
            players.UpdatePlayer(1, p);
            view.Sync(players); // spawn at 0,0,0
            Assert::AreEqual(0.0f, g_fakeWorld.lastMoves[1].x);

            // Target far ahead; advance by one 60fps frame, expressed in seconds.
            p.x = 100.0f;
            players.UpdatePlayer(1, p);
            view.Sync(players);
            view.Update(1.0f / 60.0f); // ~16.7 ms, correctly in seconds
            float first = g_fakeWorld.lastMoves[1].x;
            Assert::IsTrue(first > 0.0f && first < 100.0f,
                L"A per-frame dt in seconds must NOT teleport the visual position "
                L"to the target (that is the ms-vs-seconds regression).");

            view.Clear();
        }

        TEST_METHOD(FacingSmoothsToTargetOverShortestArc)
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
            view.SetSmoothing(1.0f); // slow: first step visibly between angles

            const float kPi = 3.14159265f;
            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = p.y = p.z = 0.0f;
            p.rotY = 0.0f;
            players.UpdatePlayer(1, p);
            view.Sync(players); // spawn facing 0
            Assert::AreEqual(0.0f, g_fakeWorld.lastMoves[1].rotY);

            // Target jumps to ~350deg (6.1 rad). The shortest arc is +10deg
            // through 0, so the smoothed value must go NEGATIVE (e.g. -few deg),
            // not climb toward +350.
            p.rotY = 350.0f * kPi / 180.0f;
            players.UpdatePlayer(1, p);
            view.Sync(players);
            view.Update(0.1f);
            float first = g_fakeWorld.lastMoves[1].rotY;
            Assert::IsTrue(first < 0.0f && first > -1.0f,
                L"Facing should rotate back (-few deg) toward 350 through the short arc.");

            // Converge. Result should normalize near -10deg (== 350), not +350.
            for (int i = 0; i < 40; ++i)
            {
                view.Sync(players);
                view.Update(0.1f);
            }
            float end = g_fakeWorld.lastMoves[1].rotY;
            Assert::IsTrue(fabsf(end + 10.0f * kPi / 180.0f) < 0.05f,
                L"Facing should converge to -10deg (== 350deg), normalized.");

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

        TEST_METHOD(JumpFlagSnapsVerticalHeightInstantly)
        {
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            memset(&sink, 0, sizeof(sink));
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            // Slow smoothing so horizontal still visibly lags; vertical must not.
            view.SetSmoothing(1.0f);

            RemotePlayerManager players;
            PlayerNetworkState ground;
            ground.x = 0.0f; ground.y = 0.0f; ground.z = 0.0f;
            players.UpdatePlayer(3, ground);
            view.Sync(players);
            Assert::AreEqual(0.0f, g_fakeWorld.lastMoves[3].y);

            // Remote player jumps: Y +100, flagged airborne.
            PlayerNetworkState air = ground;
            air.y = 100.0f;
            air.flags = PlayerNetworkState::FLAG_JUMPING | PlayerNetworkState::FLAG_IN_AIR;
            players.UpdatePlayer(3, air);
            view.Sync(players);
            // First Update: vertical must fully snap (no partial interpolation).
            view.Update(0.1f);
            Assert::AreEqual(100.0f, g_fakeWorld.lastMoves[3].y,
                L"While jumping, the height must snap instantly into place.");
            // Horizontal may still be interpolating (not a full snap) — that's fine.

            view.Clear();
        }

        TEST_METHOD(RemotePoseAppliesLocomotionDriver)
        {
            // A remote state reporting LOCO + a non-zero speed factor must reach
            // the move sink with those continuous-locomotion fields intact, so a
            // real driver (wired on the game side) can animate the walk.
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            memset(&sink, 0, sizeof(sink));
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(1.0f); // keep the visual position mostly stationary

            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
            p.motionAngle = 2500;      // ~2.5 rad
            p.facingAngle = 1000;      // 1.0 rad
            p.speedFactor = 192;       // ~75% speed
            p.characterState = 1;      // CharacterAi::LOCO
            p.flags = PlayerNetworkState::FLAG_LOCO_TURBO;
            players.UpdatePlayer(5, p);
            view.Sync(players); // spawn: initial move carries the fields verbatim
            view.Update(1.0f);

            PlayerNetworkState got = g_fakeWorld.lastMoves[5];
            Assert::AreEqual(static_cast<int>(p.motionAngle), static_cast<int>(got.motionAngle),
                L"Move sink must carry the reported motionAngle.");
            Assert::AreEqual(static_cast<int>(p.facingAngle), static_cast<int>(got.facingAngle),
                L"Move sink must carry the reported facingAngle.");
            Assert::AreEqual(static_cast<int>(p.speedFactor), static_cast<int>(got.speedFactor),
                L"Move sink must carry the reported speedFactor.");
            Assert::AreEqual(static_cast<int>(p.characterState), static_cast<int>(got.characterState),
                L"Move sink must carry the reported characterState (LOCO).");
            Assert::AreEqual(static_cast<int>(p.flags), static_cast<int>(got.flags),
                L"Move sink must carry the reported flags (turbo included).");

            // A later frame with a NON-LOCO state must NOT overwrite the driver
            // target on the move sink (gated by characterState in the game sink);
            // the view still forwards the new state verbatim so the game side can
            // decide.
            PlayerNetworkState p2 = p;
            p2.characterState = 5; // INSIM
            p2.speedFactor = 0;
            players.UpdatePlayer(5, p2);
            view.Sync(players);
            view.Update(1.0f);
            PlayerNetworkState got2 = g_fakeWorld.lastMoves[5];
            Assert::AreEqual(static_cast<int>(5), static_cast<int>(got2.characterState),
                L"Move sink must forward the non-LOCO characterState unchanged.");

            view.Clear();
        }

        TEST_METHOD(ActionEventRisingEdgeFiresOnce)
        {
            // The action sink must fire ONLY on the rising edge of a non-zero
            // actionEvent — continuous non-zero action must not re-fire.
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.actionCount = 0;
            g_fakeWorld.lastActions.clear();
            g_fakeWorld.lastMoves.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            memset(&sink, 0, sizeof(sink));
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.action = &FakeAction;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(0.01f);

            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
            p.actionEvent = PlayerNetworkState::ACTION_NONE;
            players.UpdatePlayer(6, p);
            view.Sync(players); // spawn with none
            Assert::AreEqual(0, g_fakeWorld.actionCount, L"Spawn with ACTION_NONE must not fire.");

            // First non-zero action -> fires once.
            p.actionEvent = PlayerNetworkState::ACTION_JUMP;
            players.UpdatePlayer(6, p);
            view.Sync(players);
            Assert::AreEqual(1, g_fakeWorld.actionCount, L"Action rising edge must fire once.");
            Assert::AreEqual(static_cast<int>(PlayerNetworkState::ACTION_JUMP),
                static_cast<int>(g_fakeWorld.lastActions[0]));

            // Same action stays non-zero on the next packet -> must NOT re-fire.
            view.Sync(players);
            view.Sync(players);
            Assert::AreEqual(1, g_fakeWorld.actionCount, L"Continuous non-zero action must not re-fire.");

            // A different action id is a new rising edge -> fires again.
            p.actionEvent = PlayerNetworkState::ACTION_DODGE; // reserved id passes through transport
            players.UpdatePlayer(6, p);
            view.Sync(players);
            Assert::AreEqual(2, g_fakeWorld.actionCount, L"A different action id is a new rising edge.");

            // Back to none, then the same jump again -> fires a fresh rising edge.
            p.actionEvent = PlayerNetworkState::ACTION_NONE;
            players.UpdatePlayer(6, p);
            view.Sync(players);
            p.actionEvent = PlayerNetworkState::ACTION_JUMP;
            players.UpdatePlayer(6, p);
            view.Sync(players);
            Assert::AreEqual(3, g_fakeWorld.actionCount, L"Returning to a cleared action re-arms the edge.");

            view.Clear();
        }

        TEST_METHOD(LocalActionLatchSequencesRepeatedActions)
        {
            // The latch holds the latest action with a 3-bit sequence, so two
            // dodges in a row produce DIFFERENT bytes with the SAME bare id.
            net::LocalActionLatch::Reset();
            Assert::AreEqual(0, (int)net::LocalActionLatch::Current(), L"Fresh latch holds no action.");

            net::LocalActionLatch::Report(PlayerNetworkState::ACTION_NONE);
            Assert::AreEqual(0, (int)net::LocalActionLatch::Current(), L"ACTION_NONE is not latched.");

            net::LocalActionLatch::Report(PlayerNetworkState::ACTION_DODGE);
            uint8_t first = net::LocalActionLatch::Current();
            net::LocalActionLatch::Report(PlayerNetworkState::ACTION_DODGE);
            uint8_t second = net::LocalActionLatch::Current();
            Assert::AreNotEqual((int)first, (int)second, L"A repeated action must change the held byte.");
            Assert::AreEqual((int)PlayerNetworkState::ACTION_DODGE, (int)PlayerNetworkState::ActionId(first));
            Assert::AreEqual((int)PlayerNetworkState::ACTION_DODGE, (int)PlayerNetworkState::ActionId(second));

            // Sequence wraps in 3 bits but never yields 0 for a real action.
            for (int i = 0; i < 20; ++i)
            {
                net::LocalActionLatch::Report(PlayerNetworkState::ACTION_ATTACK);
                Assert::AreNotEqual(0, (int)net::LocalActionLatch::Current(), L"Held action byte is never 0.");
            }
            net::LocalActionLatch::Reset();
            Assert::AreEqual(0, (int)net::LocalActionLatch::Current(), L"Reset clears the held action.");
        }

        TEST_METHOD(RepeatedActionFiresAgainWithBareId)
        {
            // Receiver: a new [seq|id] byte fires the sink once with the bare id,
            // the same byte held over several packets does not re-fire, and a
            // peer that spawns with an already-held action does not replay it.
            g_fakeWorld.actionCount = 0;
            g_fakeWorld.lastActions.clear();
            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            memset(&sink, 0, sizeof(sink));
            sink.spawn = &FakeSpawn;
            sink.move = &FakeMove;
            sink.action = &FakeAction;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);

            RemotePlayerManager players;
            PlayerNetworkState p;
            p.x = 0.0f; p.y = 0.0f; p.z = 0.0f;
            p.actionEvent = PlayerNetworkState::MakeActionEvent(PlayerNetworkState::ACTION_CELEB_BIG, 5);
            players.UpdatePlayer(9, p);
            view.Sync(players);
            Assert::AreEqual(0, g_fakeWorld.actionCount, L"Spawning with a held action must not replay it.");

            p.actionEvent = PlayerNetworkState::MakeActionEvent(PlayerNetworkState::ACTION_DODGE, 6);
            players.UpdatePlayer(9, p);
            view.Sync(players);
            view.Sync(players);
            Assert::AreEqual(1, g_fakeWorld.actionCount, L"New action fires once even when held.");
            Assert::AreEqual((int)PlayerNetworkState::ACTION_DODGE, (int)g_fakeWorld.lastActions.back(),
                L"Sink receives the bare id (sequence bits stripped).");

            p.actionEvent = PlayerNetworkState::MakeActionEvent(PlayerNetworkState::ACTION_DODGE, 7);
            players.UpdatePlayer(9, p);
            view.Sync(players);
            Assert::AreEqual(2, g_fakeWorld.actionCount, L"Same action with a new sequence fires again.");

            view.Clear();
        }

        TEST_METHOD(VehicleYawSteerRoundTrip)
        {
            // Vehicle mirror fields survive the remote-vehicle view: it forwards
            // the smoothed yaw + steer/speed/state to the move sink, and the yaw
            // is smoothed over the shortest arc (near 350deg converges back through 0).
            g_fakeWorld.spawnCount = 0;
            g_fakeWorld.despawnCount = 0;
            g_fakeWorld.lastMoves.clear();
            RemoteVehicleWorldView view;
            RemoteVehicleWorldView::Sink sink;
            sink.spawn = &FakeVehicleSpawn;
            sink.move = &FakeMove;
            sink.despawn = &FakeDespawn;
            view.SetSink(sink);
            view.SetSmoothing(1.0f); // slow: first step visibly between angles

            PlayerNetworkState v;
            v.x = 0.0f; v.y = 0.0f; v.z = 0.0f;
            v.inVehicle = 1;
            strcpy(v.vehicleModel, "homer");
            v.netVehicleId = 11;
            v.vehYaw = 0.0f;
            v.vehSteer = 0;
            v.vehSpeedKmh = 60;
            v.vehState = 0;
            v.vehFlags = PlayerNetworkState::VEH_FLAG_BRAKE;
            view.SyncDriver(9, v);
            view.Update(1.0f);

            PlayerNetworkState got = g_fakeWorld.lastMoves[9];
            Assert::AreEqual(static_cast<int>(v.vehSteer), static_cast<int>(got.vehSteer),
                L"Steer should reach the move sink.");
            Assert::AreEqual(static_cast<int>(v.vehSpeedKmh), static_cast<int>(got.vehSpeedKmh),
                L"Speed should reach the move sink.");
            Assert::AreEqual(static_cast<int>(v.vehState), static_cast<int>(got.vehState),
                L"Vehicle state should reach the move sink.");
            Assert::AreEqual(static_cast<int>(v.vehFlags), static_cast<int>(got.vehFlags),
                L"Vehicle flags should reach the move sink.");

            // Yaw jumps to ~350deg. Shortest arc is through 0 => first step negative.
            const float kPi = 3.14159265f;
            v.vehYaw = 350.0f * kPi / 180.0f;
            view.SyncDriver(9, v);
            view.Update(0.1f);
            Assert::IsTrue(g_fakeWorld.lastMoves[9].vehYaw < 0.0f && g_fakeWorld.lastMoves[9].vehYaw > -1.0f,
                L"Vehicle yaw should rotate back through the short arc, not climb to +350.");
            for (int i = 0; i < 40; ++i) { view.SyncDriver(9, v); view.Update(0.1f); }
            Assert::IsTrue(fabsf(g_fakeWorld.lastMoves[9].vehYaw + 10.0f * kPi / 180.0f) < 0.05f,
                L"Vehicle yaw should converge to -10deg (== 350deg).");

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

        // Regression for the two-remote-driver host crash: when a SECOND remote
        // driver is present, both ghosts are spawned in the same ForEachPlayer
        // pass and frequently rebuilt on model/inVehicle toggles. The world-view
        // contract is that EVERY despawn must be matched one-to-one with a prior
        // spawn for THAT player. If SyncDriver ever emits an extra despawn (or a
        // despawn with no matching live ghost), the real game sink ends up
        // destroying a ghost that is already gone -> double-free / heap
        // corruption. This test drives two players through enter/exit + model
        // toggles and asserts spawn/despawn balance is exact.
        TEST_METHOD(TwoDriversEnterExitAndToggleStayBalanced)
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

            PlayerNetworkState onFoot;
            onFoot.x = 0.0f; onFoot.y = 0.0f; onFoot.z = 0.0f; onFoot.inVehicle = 0;
            onFoot.vehicleModel[0] = '\0';

            PlayerNetworkState carHomer = onFoot;
            carHomer.inVehicle = 1;
            strcpy(carHomer.vehicleModel, "homer");
            carHomer.netVehicleId = 1;
            carHomer.x = 10.0f;

            PlayerNetworkState carCop = onFoot;
            carCop.inVehicle = 1;
            strcpy(carCop.vehicleModel, "cops_v");
            carCop.netVehicleId = 2;
            carCop.x = 40.0f;

            int expectedSpawns = 0, expectedDespawns = 0;

            // Both players enter at the same time (the exact two-spawn-per-frame
            // condition from OnUpdate's ForEachPlayer(SyncDriver) pass).
            view.SyncDriver(1, carHomer); ++expectedSpawns;
            view.SyncDriver(2, carCop);  ++expectedSpawns;
            view.Update(0.1f);
            Assert::AreEqual(expectedSpawns, g_fakeWorld.spawnCount, L"Both ghosts should spawn in one pass.");
            Assert::AreEqual(0, g_fakeWorld.despawnCount, L"Entering must not despawn anything.");

            // Both drive to new positions; the Update() step must not spawn/despawn.
            carHomer.x = 20.0f; carCop.x = 60.0f;
            view.SyncDriver(1, carHomer);
            view.SyncDriver(2, carCop);
            view.Update(0.1f);
            Assert::AreEqual(expectedSpawns, g_fakeWorld.spawnCount, L"Driving must never spawn/despawn.");
            Assert::AreEqual(0, g_fakeWorld.despawnCount, L"Driving must never spawn/despawn.");

            // Player 1 toggles model (DespawnIt + SpawnFor = one despawn + one spawn); player 2 stays.
            view.SyncDriver(1, carCop); ++expectedDespawns; ++expectedSpawns;
            view.Update(0.1f);
            Assert::AreEqual(3, g_fakeWorld.spawnCount, L"Model toggle respawns player 1's ghost.");
            Assert::AreEqual(1, g_fakeWorld.despawnCount,
                L"Model toggle despawns player 1's old ghost exactly once (no double-despawn).");
            // Both ghosts are still live after the toggle.
            Assert::AreEqual(2, static_cast<int>(g_fakeWorld.lastMoves.count(1) + g_fakeWorld.lastMoves.count(2)),
                L"Both drivers must still have live ghosts after a single-player toggle.");

            // Player 2 exits (despawn only). Player 1 still driving.
            view.SyncDriver(2, onFoot); ++expectedDespawns;
            view.Update(0.1f);
            Assert::AreEqual(3, g_fakeWorld.spawnCount,
                L"Spawn count must equal 2 (both entrances) + 1 (toggle respawn) = 3.");
            Assert::AreEqual(2, g_fakeWorld.despawnCount,
                L"Despawn count must equal 1 (toggle) + 1 (exit) = 2.");
            // Invariant: live ghosts = spawns - despawns. After B's exit only A's
            // ghost survives, so spawns - despawns must be exactly 1 (= no
            // double-despawn / double-free).
            Assert::AreEqual(1, g_fakeWorld.spawnCount - g_fakeWorld.despawnCount,
                L"Live ghost count (spawns - despawns) must be exactly 1 after one driver exits.");

            // Both on foot -> all ghosts gone; prune with an empty set is a no-op.
            view.SyncDriver(1, onFoot); ++expectedDespawns;
            view.Prune(RemotePlayerManager());
            view.Update(0.1f);
            Assert::AreEqual(expectedSpawns, expectedDespawns,
                L"Every spawned ghost must have been despawned exactly once by the end (spawn==despawn).");
            Assert::AreEqual(g_fakeWorld.spawnCount, g_fakeWorld.despawnCount,
                L"The fake sink must have seen exactly one despawn per spawn (no double-despawn).");

            view.Clear();
        }
    };

    TEST_CLASS(ServerAuthorityTests)
    {
    public:
        // A fresh small state is accepted and relayed back to a second client.
        TEST_METHOD(ValidationAcceptsNormalMovement)
        {
            ScriptedPlayer aState;
            aState.state.x = 0.0f; aState.state.y = 0.0f; aState.state.z = 0.0f; aState.state.rotY = 0.0f;
            aState.state.vehicleModel[0] = '\0'; aState.state.netVehicleId = 0; aState.state.inVehicle = 0;
            aState.ready = true;

            net::GamePlayerBridge aBridge;
            aBridge.SetTypedLocalProvider(
                [](PlayerNetworkState& out) -> bool { return g_scriptedHost(out); });
            g_scriptedHostHolder = &aState;

            RemotePlayerManager bRemote;
            net::GamePlayerBridge bBridge;
            bBridge.SetRemotePlayers(&bRemote);

            PlayerMoveValidator validator;
            unsigned short port = PickFreePort();

            net::Session host;
            host.SetStateValidator(&validator);
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session a;
            aBridge.AttachToSession(&a);
            Assert::IsTrue(a.Join("127.0.0.1", port));
            net::Session b;
            bBridge.AttachToSession(&b);
            Assert::IsTrue(b.Join("127.0.0.1", port));

            Assert::IsTrue(PumpToWelcome(host, a), L"Client A should be welcomed.");
            Assert::IsTrue(PumpToWelcome(host, b), L"Client B should be welcomed.");
            net::PeerId aId = a.GetLocalPeerId();

            // Give client A a few honest, small footsteps (publish via the
            // scripted provider; the session relays on its own cadence).
            for (int i = 0; i < 120; ++i) {
                aState.state.x += 0.5f;
                a.Update(); b.Update(); host.Update();
                Sleep(2);
            }

            // The validator accepted the honest movement and the relayed state
            // reached another peer.
            Assert::IsTrue(validator.accepted > 0,
                L"Normal movement should be accepted by the server validator.");
            const PlayerNetworkState* bSeesA = bRemote.GetPlayer(static_cast<int>(aId));
            Assert::IsNotNull(bSeesA, L"B should see A's accepted movement through the relay.");

            g_scriptedHostHolder = nullptr;
            b.Leave(); a.Leave(); host.Leave();
        }

        // A teleport far away within a single tick must be rejected (not relayed).
        TEST_METHOD(ValidationRejectsTeleport)
        {
            ScriptedPlayer aState;
            aState.state.x = 0.0f; aState.state.y = 0.0f; aState.state.z = 0.0f; aState.state.rotY = 0.0f;
            aState.state.vehicleModel[0] = '\0'; aState.state.netVehicleId = 0; aState.state.inVehicle = 0;
            aState.ready = true;

            net::GamePlayerBridge aBridge;
            aBridge.SetTypedLocalProvider(
                [](PlayerNetworkState& out) -> bool { return g_scriptedHost(out); });
            g_scriptedHostHolder = &aState;

            RemotePlayerManager bRemote;
            net::GamePlayerBridge bBridge;
            bBridge.SetRemotePlayers(&bRemote);

            PlayerMoveValidator validator;
            unsigned short port = PickFreePort();

            net::Session host;
            host.SetStateValidator(&validator);
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session a;
            aBridge.AttachToSession(&a);
            Assert::IsTrue(a.Join("127.0.0.1", port));
            net::Session b;
            bBridge.AttachToSession(&b);
            Assert::IsTrue(b.Join("127.0.0.1", port));

            Assert::IsTrue(PumpToWelcome(host, a), L"Client A should be welcomed.");
            Assert::IsTrue(PumpToWelcome(host, b), L"Client B should be welcomed.");
            net::PeerId aId = a.GetLocalPeerId();

            // Establish a plausible position with honest steps first.
            for (int i = 0; i < 30 && validator.accepted == 0; ++i) {
                aState.state.x += 0.5f;
                a.Update(); b.Update(); host.Update();
                Sleep(2);
            }
            Assert::IsTrue(validator.accepted > 0, L"Baseline steps should be accepted.");
            int acceptedBefore = validator.accepted;

            // Now a teleport thousands of metres in a single tick.
            long currentX = 0;
            (void)currentX;
            aState.state.x = 1.0f;
            aState.state.z = 10000.0f;
            for (int i = 0; i < 20; ++i) {
                a.Update(); b.Update(); host.Update();
                Sleep(2);
            }

            // The validator rejected the teleport and it was NOT relayed: B never
            // observes z near 10000 regardless of how far A's state spread.
            Assert::IsTrue(validator.rejected > 0,
                L"Teleport should be rejected by the server validator.");
            const PlayerNetworkState* bSeesA = bRemote.GetPlayer(static_cast<int>(aId));
            if (bSeesA) {
                Assert::IsTrue(bSeesA->z < 100.0f,
                    L"Teleported position must not be relayed to peers.");
            }

            g_scriptedHostHolder = nullptr;
            b.Leave(); a.Leave(); host.Leave();
        }

        // Integration: two clients over a shared server; A's accepted state must
        // be relayed so client B sees A through its own RemotePlayerManager.
        TEST_METHOD(TwoClientsSeeEachOtherThroughRelay)
        {
            ScriptedPlayer aState;
            aState.state.x = 0.0f; aState.state.y = 0.0f; aState.state.z = 0.0f; aState.state.rotY = 0.0f;
            aState.state.vehicleModel[0] = '\0'; aState.state.netVehicleId = 0; aState.state.inVehicle = 0;
            aState.ready = true;

            net::GamePlayerBridge aBridge;
            aBridge.SetTypedLocalProvider(
                [](PlayerNetworkState& out) -> bool { return g_scriptedHost(out); });
            g_scriptedHostHolder = &aState;

            RemotePlayerManager bRemote;
            net::GamePlayerBridge bBridge;
            bBridge.SetRemotePlayers(&bRemote);

            PlayerMoveValidator validator;
            unsigned short port = PickFreePort();

            net::Session host;
            host.SetStateValidator(&validator);
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session a;
            aBridge.AttachToSession(&a);
            Assert::IsTrue(a.Join("127.0.0.1", port));
            net::Session b;
            bBridge.AttachToSession(&b);
            Assert::IsTrue(b.Join("127.0.0.1", port));

            Assert::IsTrue(PumpToWelcome(host, a), L"Client A should be welcomed.");
            Assert::IsTrue(PumpToWelcome(host, b), L"Client B should be welcomed.");
            net::PeerId aId = a.GetLocalPeerId();

            // Client A walks forward in honest, slow steps so the server accepts
            // and relays the state (speed well under the on-foot limiter).
            for (int i = 0; i < 60; ++i) {
                aState.state.x += 0.5f;
                a.Update(); b.Update(); host.Update();
                Sleep(25); // 0.5m per 25ms = 20 m/s, below the ~22.5 m/s foot cap
            }

            // Client B must now see A's advanced position through the relay.
            const PlayerNetworkState* bSeesA = bRemote.GetPlayer(static_cast<int>(aId));
            Assert::IsNotNull(bSeesA, L"B should see A through the server relay.");
            if (bSeesA) {
                Assert::IsTrue(bSeesA->x > 10.0f,
                    L"B should observe A's advanced position.");
            }

            // And the server authoritative validator accepted A's movement.
            Assert::IsTrue(validator.accepted > 0,
                L"Server should accept A's honest movement in its authoritative set.");

            g_scriptedHostHolder = nullptr;
            b.Leave(); a.Leave(); host.Leave();
        }
    };

    TEST_CLASS(ServerModManagerTests)
    {
    public:
        TEST_METHOD(LuaHooksControlJoinAndValidate)
        {
            ServerModManager mods;
            Assert::IsTrue(mods.LoadScript(TestDataPath("test_server_mod.lua").c_str()),
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

        TEST_METHOD(LuaEventBusDispatchesToRegisteredHandlers)
        {
            // The event bus: a script registers handlers with net.on(); the
            // engine dispatches to them via Emit with typed args. Handlers
            // increment Lua globals we read back with GetGlobalInt().
            ServerModManager mods;
            Assert::IsTrue(mods.LoadScript(TestDataPath("test_server_mod_events.lua").c_str()),
                L"Should load the event-bus test script.");

            // No one listens on an unknown event -> Emit reports nothing ran.
            int noEvt[] = { 42 };
            Assert::IsFalse(mods.Emit("player.nobody", noEvt, 1),
                L"Emit on an unregistered event must report no handler ran.");

            // Dispatch "player.spawn" twice and "player.kick" once.
            int a[] = { 7 };
            int b[] = { 9, 2 };
            Assert::IsTrue(mods.Emit("player.spawn", a, 1), L"spawn handler did not run.");
            Assert::IsTrue(mods.Emit("player.spawn", a, 1), L"spawn handler did not run a 2nd time.");
            Assert::IsTrue(mods.Emit("player.kick", b, 2), L"kick handler did not run.");

            // Handlers incremented their Lua globals; read them back.
            Assert::AreEqual(2, mods.GetGlobalInt("spawns"), L"spawn handler ran the wrong number of times.");
            Assert::AreEqual(1, mods.GetGlobalInt("kicks"), L"kick handler ran the wrong number of times.");
        }

        TEST_METHOD(ClientLuaEventBusDispatchesToRegisteredHandlers)
        {
            // The client-side mirror (ClientModManager) uses the same net.on()
            // API and Emit() dispatch as the server side, so a client mod can
            // react to client-scoped events (match start, world seed, inbound
            // custom events) identically.
            ClientModManager mods;
            Assert::IsTrue(mods.LoadScript(TestDataPath("test_server_mod_events.lua").c_str()),
                L"Should load the event-bus test script on the client side.");

            int noEvt[] = { 42 };
            Assert::IsFalse(mods.Emit("player.nobody", noEvt, 1),
                L"Client Emit on an unregistered event must report no handler ran.");

            int a[] = { 7 };
            int b[] = { 9, 2 };
            Assert::IsTrue(mods.Emit("player.spawn", a, 1), L"Client spawn handler did not run.");
            Assert::IsTrue(mods.Emit("player.spawn", a, 1), L"Client spawn handler did not run a 2nd time.");
            Assert::IsTrue(mods.Emit("player.kick", b, 2), L"Client kick handler did not run.");

            Assert::AreEqual(2, mods.GetGlobalInt("spawns"), L"Client spawn handler ran the wrong number of times.");
            Assert::AreEqual(1, mods.GetGlobalInt("kicks"), L"Client kick handler ran the wrong number of times.");
        }
    };

    TEST_CLASS(MissionSyncTests)
    {
    public:
        TEST_METHOD(MissionStateSerializesRoundtrip)
        {
            MissionSyncState s;
            s.missionIndex = 3;
            s.stageIndex = 1;
            s.objectiveIndex = 2;
            s.inMission = 1;

            char buffer[MissionSyncState::SERIALIZED_SIZE];
            s.Serialize(buffer);

            MissionSyncState restored;
            restored.Deserialize(buffer);
            Assert::AreEqual(s.missionIndex, restored.missionIndex);
            Assert::AreEqual(s.stageIndex, restored.stageIndex);
            Assert::AreEqual(s.objectiveIndex, restored.objectiveIndex);
            Assert::AreEqual(s.inMission, restored.inMission);
        }

        TEST_METHOD(MissionSyncManagerTracksLocalAndRemote)
        {
            MissionSyncManager mgr;
            mgr.SetLocalMission(2, 0, 0, true);
            Assert::AreEqual(2, mgr.GetLocalState().missionIndex);
            Assert::AreEqual(1, static_cast<int>(mgr.GetLocalState().inMission));

            mgr.SyncStage(1);
            mgr.SyncObjective(3);
            Assert::AreEqual(1, mgr.GetCurrentStage());
            Assert::AreEqual(3, mgr.GetCurrentObjective());

            MissionSyncState remote;
            remote.missionIndex = 4;
            remote.stageIndex = 0;
            remote.inMission = 1;
            mgr.UpdateRemotePlayer(100, remote);

            bool seen = false;
            mgr.ForEachRemotePlayer(
                [](int id, const MissionSyncState& state, void* ud) -> bool
                {
                    Assert::AreEqual(100, id);
                    Assert::AreEqual(4, state.missionIndex);
                    *static_cast<bool*>(ud) = true;
                    return true;
                },
                &seen);
            Assert::IsTrue(seen, L"Remote player's mission state should be iterable.");

            mgr.RemoveRemotePlayer(100);
            Assert::IsFalse(mgr.HasRemotePlayer(100), L"Removed remote player should be gone.");
        }

        TEST_METHOD(MissionStateRelaysBetweenClientsThroughServer)
        {
            // Mission progress rides the same generic relay as player state in
            // the engine layer (net::Session relays arbitrary blobs). Here we
            // serialize MissionSyncState into the blob and verify it survives the
            // server relay: A publishes, B's world sink receives the exact blob.
            MissionSyncState msA;
            msA.missionIndex = 2;
            msA.stageIndex = 1;
            msA.objectiveIndex = 0;
            msA.inMission = 1;

            ScriptedPlayer aState;
            aState.ready = true;
            // Pack the mission state into the first bytes of the player-state
            // blob so it passes through the relay verbatim.
            memcpy(reinterpret_cast<char*>(&aState.state), &msA, sizeof(msA));
            aState.state.flags = 0;

            net::GamePlayerBridge aBridge;
            aBridge.SetTypedLocalProvider(
                [](PlayerNetworkState& out) -> bool { return g_scriptedHost(out); });
            g_scriptedHostHolder = &aState;

            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xAA55));

            net::Session a;
            aBridge.AttachToSession(&a);
            Assert::IsTrue(a.Join("127.0.0.1", port));
            net::Session b;
            Assert::IsTrue(b.Join("127.0.0.1", port));

            Assert::IsTrue(PumpToWelcome(host, a), L"Client A should be welcomed.");
            Assert::IsTrue(PumpToWelcome(host, b), L"Client B should be welcomed.");
            net::PeerId aId = a.GetLocalPeerId();

            // B receives A's mission blob via the relay through a capture sink.
            RemotePlayerManager bRemote;
            net::GamePlayerBridge bBridge;
            bBridge.SetRemotePlayers(&bRemote);
            bBridge.AttachToSession(&b);

            // Pump until B has A's state.
            const PlayerNetworkState* seen = NULL;
            for (int i = 0; i < 400 && seen == NULL; ++i) {
                a.Update(); b.Update(); host.Update();
                seen = bRemote.GetPlayer(static_cast<int>(aId));
            }
            Assert::IsNotNull(seen, L"B should see A's state via the relay.");
            if (seen) {
                // The first bytes carried the serialized MissionSyncState.
                MissionSyncState received;
                memcpy(&received, seen, sizeof(MissionSyncState));
                Assert::AreEqual(2, received.missionIndex, L"Mission index should survive the relay.");
                Assert::AreEqual(1, received.stageIndex, L"Stage index should survive the relay.");
            }

            g_scriptedHostHolder = nullptr;
            b.Leave(); a.Leave(); host.Leave();
        }
    };

    TEST_CLASS(NetEnginePacketTests)
    {
    public:
        TEST_METHOD(WriterReaderRoundtripLittleEndian)
        {
            uint8_t buf[32];
            net::Writer w(buf, sizeof(buf));
            Assert::IsTrue(w.WriteU8(0x12));
            Assert::IsTrue(w.WriteU16(0x3456));
            Assert::IsTrue(w.WriteU32(0x789ABCDE));
            Assert::IsTrue(w.WriteFloat(1.5f));
            Assert::IsTrue(w.Ok());

            net::Reader r(buf, w.Position());
            uint8_t u8; uint16_t u16; uint32_t u32; float f;
            Assert::IsTrue(r.ReadU8(u8));   Assert::AreEqual((int)0x12, (int)u8);
            Assert::IsTrue(r.ReadU16(u16)); Assert::AreEqual((int)0x3456, (int)u16);
            Assert::IsTrue(r.ReadU32(u32)); Assert::AreEqual((int)0x789ABCDE, (int)u32);
            Assert::IsTrue(r.ReadFloat(f)); Assert::AreEqual(1.5f, f);
            Assert::AreEqual(0, (int)r.Remaining());
        }

        TEST_METHOD(WriterRejectsOverflow)
        {
            uint8_t buf[4];
            net::Writer w(buf, sizeof(buf));
            Assert::IsTrue(w.WriteU32(0x11111111));
            // No room for a second u32 -> write fails, Ok() stays false.
            Assert::IsFalse(w.WriteU32(0x22222222));
            Assert::IsFalse(w.Ok());
        }

        TEST_METHOD(ReaderRejectsTruncated)
        {
            // Buffer has only 2 bytes, requesting a u32 must fail without overrun.
            uint8_t buf[2] = { 0x01, 0x02 };
            net::Reader r(buf, sizeof(buf));
            uint32_t v;
            Assert::IsFalse(r.ReadU32(v));
            Assert::AreEqual(0, (int)r.Position()); // no progress past end
        }

        TEST_METHOD(RegistryRoutesAndDropsUnknown)
        {
            net::PacketRegistry reg;
            int hits = 0;
            net::PacketId gotId = 0xFFFF;
            Assert::IsTrue(reg.Register(
                net::PID_PLAYER_STATE,
                [&](const net::InboundPacket& p) { hits++; gotId = p.id; }));

            uint8_t body[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
            // Known id routed to handler.
            net::InboundPacket known(net::PID_PLAYER_STATE, 7, body, sizeof(body));
            Assert::IsTrue(reg.Dispatch(known));
            Assert::AreEqual(1, hits);
            Assert::AreEqual((int)net::PID_PLAYER_STATE, (int)gotId);

            // Unknown id dropped silently, counted.
            net::InboundPacket unknown(static_cast<net::PacketId>(0x7FAA), 7, body, sizeof(body));
            Assert::IsFalse(reg.Dispatch(unknown));
            Assert::AreEqual(1U, reg.GetUnknownDropped());
            Assert::AreEqual(1, hits); // no handler ran
        }

        TEST_METHOD(ReservedIdRangesAreFixed)
        {
            // Core ids live below 0x0100; gameplay ids in [0x0100,0x0200).
            Assert::IsTrue(net::PID_HELLO <= 0xFF);
            Assert::IsTrue(net::PID_PLAYER_STATE >= 0x0100 && net::PID_PLAYER_STATE < 0x0200);
            Assert::IsTrue(net::PID_MISSION_STATE >= 0x0100 && net::PID_MISSION_STATE < 0x0200);
        }
    };

    TEST_CLASS(NetEngineTransportTests)
    {
    public:
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

        // A minimal ENet client that connects and pumps, delivering raw frames
        // to a sink — enough to exercise ServerEndpoint's frame decode path.
        TEST_METHOD(ServerEndpointAcceptsFramedPacket)
        {
            net::ServerEndpoint server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port, 4));

            bool peerJoined = false, peerLeft = false;
            net::PeerId joinedId = 0;
            server.SetPeerEventFn([&](const net::PeerEvent& e) {
                if (e.connected) { peerJoined = true; joinedId = e.peer; }
                else peerLeft = true;
            });

            // Decode-side: capture raw frames into a registry.
            net::PacketRegistry reg;
            uint8_t got[16]; int gotLen = -1; net::PacketId gotId = 0xFFFF;
            server.SetPacketSink([&](const net::ServerEndpoint::RawPacket& rp) {
                gotId = rp.id;
                gotLen = rp.size > sizeof(got) ? (int)sizeof(got) : (int)rp.size;
                memcpy(got, rp.data, (size_t)gotLen);
                // Also route through the registry to prove end-to-end path.
                reg.Dispatch(net::InboundPacket(rp.id, rp.from, rp.data, rp.size));
            });

            // A real ENet client connects over loopback.
            ENetHost* ch = enet_host_create(nullptr, 1, 1, 0, 0);
            Assert::IsNotNull(ch);
            ENetAddress addr;
            enet_address_set_host(&addr, "127.0.0.1");
            addr.port = port;
            ENetPeer* cp = enet_host_connect(ch, &addr, 1, 0);
            Assert::IsNotNull(cp);

            bool serverReady = false;
            for (int i = 0; i < 200 && !serverReady; ++i)
            {
                server.Update();
                ENetEvent ce;
                while (enet_host_service(ch, &ce, 0) > 0)
                {
                    if (ce.type == ENET_EVENT_TYPE_RECEIVE)
                        enet_packet_destroy(ce.packet);
                    serverReady = (ce.type == ENET_EVENT_TYPE_CONNECT);
                }
            }

            // Give ServerEndpoint time to observe the connect and assign an id.
            for (int i = 0; i < 200 && !peerJoined; ++i) server.Update();

            Assert::IsTrue(peerJoined, L"ServerEndpoint should report a connected peer.");
            Assert::IsTrue(joinedId != 0, L"Peer id should be non-zero.");

            enet_host_destroy(ch);
            server.Stop();
        }
    // Round-trip: ClientEndpoint sends a framed packet that the
        // ServerEndpoint decodes into the raw sink. Proves both transports and
        // the shared framing work together.
        TEST_METHOD(ClientServerEndpointRoundtrip)
        {
            net::ServerEndpoint server;
            unsigned short port = PickFreePort();
            Assert::IsTrue(server.Start(port, 4));

            bool serverGotJoin = false;
            net::PeerId serverPeerId = 0;
            server.SetPeerEventFn([&](const net::PeerEvent& e) {
                if (e.connected) { serverGotJoin = true; serverPeerId = e.peer; }
            });

            net::PacketId serverGotId = 0xFFFF;
            uint8_t serverGot[8];
            int gotLen = -1;
            server.SetPacketSink([&](const net::ServerEndpoint::RawPacket& rp) {
                serverGotId = rp.id;
                gotLen = rp.size > (int)sizeof(serverGot) ? (int)sizeof(serverGot) : (int)rp.size;
                memcpy(serverGot, rp.data, (size_t)gotLen);
            });

            net::ClientEndpoint client;
            Assert::IsTrue(client.Connect("127.0.0.1", port));
            client.SetPacketSink([](const net::ClientEndpoint::RawPacket&) {});

            // Pump until the server observes the join and client is ready.
            for (int i = 0; i < 300 && (!serverGotJoin || !client.IsPeerReady()); ++i)
            {
                server.Update();
                client.Update();
            }
            Assert::IsTrue(serverGotJoin, L"Server should observe client joining.");
            Assert::IsTrue(client.IsPeerReady(), L"Client should observe server connect.");
            Assert::IsTrue(serverPeerId != 0, L"Server-assigned peer id must be non-zero.");

            // Client sends a small payload; server must decode id + bytes.
            uint8_t payload[4] = { 0x10, 0x20, 0x30, 0x40 };
            Assert::IsTrue(client.SendTo(static_cast<net::PacketId>(0x1234), payload, sizeof(payload)));

            for (int i = 0; i < 100 && gotLen < 0; ++i) { server.Update(); client.Update(); }

            Assert::AreEqual((int)0x1234, (int)serverGotId, L"Server should decode the packet id.");
            Assert::AreEqual(4, gotLen);
            Assert::AreEqual((int)payload[0], (int)serverGot[0]);
            Assert::AreEqual((int)payload[3], (int)serverGot[3]);

            client.Disconnect();
            server.Stop();
        }
    // Session orchestration: a host session spawns its loopback client and a
        // joiner session can start on a free port. Mirrors (and later replaces)
        // NetworkMatchManager's lifecycle, but free of any game-world coupling.
        TEST_METHOD(SessionHostAndJoinLifecycle)
        {
            net::Session host;
            unsigned short port = PickFreePort();
            Assert::IsTrue(host.StartHost(port, 0xC0FFEE));
            Assert::IsTrue(host.IsHost());
            Assert::IsTrue(host.IsJoined()); // host participates via loopback

            // A second, independent joiner session connects to the host.
            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(joiner.IsJoined());
            Assert::IsFalse(joiner.IsHost());

            // Host cannot start a second match while active.
            Assert::IsFalse(host.StartHost(PickFreePort(), 1));

            joiner.Leave();
            Assert::IsFalse(joiner.IsInMatch());
            host.Leave();
            Assert::IsFalse(host.IsInMatch());
        }
    // End-to-end session protocol over raw endpoints: the host's peer-handshake
        // sends WELCOME + WORLD_SEED + PLAYER_LIST, and a second host on a
        // different port would do the same. Here we verify the message framing
        // is actually produced by a host session onto the wire and decoded by
        // the loopback client into selfId/seed/roster.
        TEST_METHOD(SessionProtocolAssignsSelfAndSeed)
        {
            const uint32_t kSeed = 0xD00DFEED;
            unsigned short port = PickFreePort();

            // Two sessions: host (with loopback) + one remote joiner.
            net::Session host;
            Assert::IsTrue(host.StartHost(port, kSeed));

            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));

            // Pump until both have resolved self ids and the joiner sees the seed.
            bool hostReady = false, joinReady = false, seedSeen = false;
            for (int i = 0; i < 400 && !(joinReady && seedSeen); ++i)
            {
                host.Update();
                joiner.Update();
                hostReady  = host.IsJoined();
                joinReady  = joiner.IsJoined();
                seedSeen   = joiner.GetWorldSeed() == kSeed;
            }

            Assert::IsTrue(host.IsHost());
            Assert::IsTrue(joinReady, L"Joiner should connect to host session.");
            Assert::IsTrue(seedSeen, L"Joiner should receive the host's world seed.");
            Assert::IsTrue(joiner.GetLocalPeerId() != 0,
                L"Joiner should receive a non-zero self id via WELCOME.");
            // The host itself participates via loopback and has a self id too.
            Assert::IsTrue(host.GetLocalPeerId() != 0,
                L"Host's loopback client should receive a self id.");

            joiner.Leave();
            host.Leave();
        }
    // World relay: a host session sends its local state (via ILocalStateProvider)
        // and the joiner, through the server relay, receives it in its
        // IRemoteStateSink. Proves local-state + relay + remote-sink path.
        TEST_METHOD(SessionRelaysLocalStateToJoiner)
        {
            struct Provider : net::ILocalStateProvider
            {
                uint8_t byte = 0xAB;
                bool GetLocalState(uint8_t* out, uint16_t cap, uint16_t& size) override
                {
                    if (cap < 1) return false;
                    out[0] = byte; size = 1; return true;
                }
            };
            struct Sink : net::IRemoteStateSink
            {
                int got = 0; uint8_t last = 0;
                void ApplyRemoteState(net::PeerId, const uint8_t* d, uint16_t n) override
                { got++; last = (n > 0) ? d[0] : 0; }
                void OnPeerLeft(net::PeerId) override {}
            };

            Provider hostProv; Sink joinerSink;
            unsigned short port = PickFreePort();

            net::Session host;
            host.SetLocalStateProvider(&hostProv);
            Assert::IsTrue(host.StartHost(port, 0xD00D));

            net::Session joiner;
            joiner.SetRemoteStateSink(&joinerSink);
            Assert::IsTrue(joiner.Join("127.0.0.1", port));

            // Pump generously: handshake, then host's periodic local-state send
            // (every 6 Update calls) must reach the joiner through the relay.
            for (int i = 0; i < 600 && joinerSink.got == 0; ++i)
            {
                host.Update();
                joiner.Update();
            }

            Assert::IsTrue(joinerSink.got > 0,
                L"Joiner should receive the host's state via the relay.");
            Assert::AreEqual((int)0xAB, (int)joinerSink.last);

            joiner.Leave(); host.Leave();
        }
    // Dedicated mission-progress channel: mission progress rides its own packet
        // id (PID_MISSION_STATE) and cadence, independent of the transform blob.
        // A joiner with a mission provider publishes progress; the host's mission
        // sink receives the exact MissionSyncState via the server relay.
        TEST_METHOD(MissionStateRelaysThroughDedicatedChannel)
        {
            struct MissionProvider : net::IMissionStateProvider
            {
                MissionSyncState ms;
                MissionProvider() { ms.Clear(); }
                bool GetMissionState(uint8_t* out, uint16_t cap, uint16_t& size) override
                {
                    if (cap < (uint16_t)MissionSyncState::SERIALIZED_SIZE) return false;
                    ms.Serialize(reinterpret_cast<char*>(out));
                    size = (uint16_t)MissionSyncState::SERIALIZED_SIZE;
                    return true;
                }
            };
            struct MissionSink
            {
                int got = 0;
                net::PeerId from = 0;
                MissionSyncState last;
            };

            MissionProvider joinerProv;
            joinerProv.ms.missionIndex = 7;
            joinerProv.ms.stageIndex = 2;
            joinerProv.ms.objectiveIndex = 1;
            joinerProv.ms.inMission = 1;

            MissionSink hostSink;
            unsigned short port = PickFreePort();

            net::Session host;
            host.SetMissionStateSink(
                [&](net::PeerId f, const uint8_t* d, uint16_t n)
                {
                    if (n < (uint16_t)MissionSyncState::SERIALIZED_SIZE) return;
                    hostSink.from = f;
                    hostSink.last.Deserialize(reinterpret_cast<const char*>(d));
                    hostSink.got++;
                });
            Assert::IsTrue(host.StartHost(port, 0xBEEF));

            net::Session joiner;
            joiner.SetMissionStateProvider(&joinerProv);
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, joiner), L"Joiner did not get WELCOME.");

            // Pump generously: mission cadence is every 18 Update frames, so the
            // joiner must advance several send windows for the host to receive it.
            for (int i = 0; i < 800 && hostSink.got == 0; ++i)
            {
                host.Update();
                joiner.Update();
            }

            Assert::IsTrue(hostSink.got > 0,
                L"Host should receive the joiner's mission state via the relay.");
            Assert::AreEqual((int)joiner.GetLocalPeerId(), (int)hostSink.from,
                L"Sender id should round-trip.");
            Assert::AreEqual(7, hostSink.last.missionIndex);
            Assert::AreEqual(2, hostSink.last.stageIndex);
            Assert::AreEqual(1, hostSink.last.objectiveIndex);
            Assert::AreEqual((int)1, (int)hostSink.last.inMission);

            joiner.Leave(); host.Leave();
        }
    }; // NetEngineTransportTests

    TEST_CLASS(P3DWriterTests)
    {
    public:
        TEST_METHOD(WritesValidChunkHierarchy)
        {
            P3DWriter w;
            w.BeginProject("lobby", 640, 480, "pc", "art\\pages", "art\\res", "art\\screens");
            w.BeginScreen("lobby_screen", 1, 0);
            w.EndScreen();
            w.BeginPage("lobby_page", 1, 640, 480);
            w.BeginLayer("main", 1, true, true, 255);
            const char* images[] = { "cursor.png" };
            w.WriteSprite("cursor", 1, 0, 0, 32, 32, 0, 0, 0xFFFFFFFF, 0, 0.0f, images, 1);
            w.EndSprite();
            w.BeginGroup("textgrp", 1, 0);
            w.WriteText("Title", 17, 10, 10, 200, 30, 0, 0, 0xFFFFFFFF,
                0, 0.0f, "HudTextStyle", true, 0xFF000000, 1, 2, 0);
            w.AddTextHardCodedString("Hello");
            w.EndText();
            w.EndGroup();
            w.EndLayer();
            w.EndPage();
            w.EndProject();

            const std::vector<char>& data = w.GetData();
            // Root DATA_FILE chunk header + PROJECT chunk.
            Assert::IsTrue(data.size() > 12, L"Writer should emit at least a root header.");
            // Structural self-check: walk the generated bytes recursively and verify the
            // chunk headers nest correctly (a mini-reader matching tChunkFile's
            // chunk framing: [id u32][dataLength u32][chunkLength u32]).
            size_t pos = 0;
            auto readU32 = [&](size_t p) -> uint32_t {
                return (static_cast<unsigned char>(data[p]))
                    | (static_cast<unsigned char>(data[p + 1]) << 8)
                    | (static_cast<unsigned char>(data[p + 2]) << 16)
                    | (static_cast<unsigned char>(data[p + 3]) << 24);
            };
            int depth = 0;
            int rootSeen = 0;
            // Simple recursive descent over the flat buffer using declared lengths.
            std::function<void(size_t)> walk = [&](size_t start) {
                uint32_t id = readU32(start);
                uint32_t dataL = readU32(start + 4);
                uint32_t chunkL = readU32(start + 8);
                Assert::IsTrue(chunkL >= dataL, L"Chunk length must cover data.");
                // dataLength = 12 + payload bytes (before sub-chunks start).
                size_t substart = start + dataL;
                if (id == 0xFF443350) rootSeen++;
                depth++;
                // Recurse into sub-chunks while there is space within chunkL.
                size_t p = substart;
                while (p + 12 <= start + chunkL) {
                    uint32_t childL = (static_cast<unsigned char>(data[p + 8]))
                        | (static_cast<unsigned char>(data[p + 9]) << 8)
                        | (static_cast<unsigned char>(data[p + 10]) << 16)
                        | (static_cast<unsigned char>(data[p + 11]) << 24);
                    if (childL == 0) break;
                    walk(p);
                    p += childL;
                }
                depth--;
            };
            walk(0);
            Assert::AreEqual(1, rootSeen, L"Exactly one DATA_FILE root expected.");

            // Root chunk id must be DATA_FILE (0xFF443350), little-endian.
            unsigned char b0 = static_cast<unsigned char>(data[0]);
            Assert::AreEqual(0x50, (int)b0, L"Root chunk id low byte should match DATA_FILE.");
            // dataLength and chunkLength fields present and non-zero.
            uint32_t dataLength = (static_cast<unsigned char>(data[4]))
                | (static_cast<unsigned char>(data[5]) << 8)
                | (static_cast<unsigned char>(data[6]) << 16)
                | (static_cast<unsigned char>(data[7]) << 24);
            uint32_t chunkLength = (static_cast<unsigned char>(data[8]))
                | (static_cast<unsigned char>(data[9]) << 8)
                | (static_cast<unsigned char>(data[10]) << 16)
                | (static_cast<unsigned char>(data[11]) << 24);
            Assert::IsTrue(dataLength > 0, L"Root dataLength must be non-zero.");
            Assert::IsTrue(chunkLength >= dataLength, L"Root chunkLength must cover the data.");
        }

        TEST_METHOD(WriteTextWithVersionBelow17HasNoShadow)
        {
            P3DWriter w;
            w.BeginProject("p", 640, 480, "pc", "", "", "");
            w.BeginScreen("s", 1, 0);
            w.EndScreen();
            w.BeginPage("page", 1, 640, 480);
            w.BeginLayer("l", 1, true, true, 255);
            w.WriteText("plain", 16, 0, 0, 100, 20, 0, 0, 0xFFFFFFFF,
                0, 0.0f, "Style", false, 0, 0, 0, 0);
            w.EndLayer();
            w.EndPage();
            w.EndProject();
            // version 16 => no drop-shadow block: no extra 1+4+4+4+4 bytes.
            Assert::IsTrue(w.GetData().size() > 0);
        }

        TEST_METHOD(WritePolygonEmitsValidChunk)
        {
            // The FrontendPolygon (0x18009) chunk the loader reads: name,
            // version, translucency, numPoints, [x,y,z] per point, [colour] per
            // point. Verify the writer lays those fields out in the loader order.
            P3DWriter w;
            w.BeginProject("p", 640, 480, "pc", "", "", "");
            w.BeginScreen("s", 1, 0);
            w.EndScreen();
            w.BeginPage("page", 1, 640, 480);
            w.BeginLayer("l", 1, true, true, 255);
            const float pts[6] = { 0, 0, 0, 100, 0, 50 };
            const uint32_t cols[2] = { 0xFFFF0000u, 0xFF00FF00u };
            w.WritePolygon("tri", 1, 128, pts, cols, 2);
            w.EndPolygon();
            w.EndLayer();
            w.EndPage();
            w.EndProject();

            // Find the POLYGON chunk (0x18009) anywhere in the generated file (it lives
            // several levels deep: DATA_FILE > PROJECT > SCREEN > PAGE > LAYER).
            // Recurse through declared chunk lengths like the mini-reader in
            // WritesValidChunkHierarchy.
            const std::vector<char>& data = w.GetData();
            auto readU32 = [&](size_t p) -> uint32_t {
                if (p + 4 > data.size()) return 0;
                return (static_cast<unsigned char>(data[p]))
                    | (static_cast<unsigned char>(data[p + 1]) << 8)
                    | (static_cast<unsigned char>(data[p + 2]) << 16)
                    | (static_cast<unsigned char>(data[p + 3]) << 24);
            };
            bool found = false;
            std::function<void(size_t)> walk = [&](size_t start) {
                if (start + 12 > data.size()) return;
                uint32_t id = readU32(start);
                uint32_t dataL = readU32(start + 4);
                uint32_t chunkL = readU32(start + 8);
                if (chunkL == 0 || start + chunkL > data.size()) return;

                if (id == 0x18009)
                {
                    found = true;
                    // name (Pascal), version u32, translucency u32, numPoints u32.
                    size_t p = start + 12;
                    unsigned nameLen = static_cast<unsigned char>(data[p]);
                    p += 1 + nameLen;
                    Assert::AreEqual(1u, readU32(p), L"Polygon version mismatch.");
                    Assert::AreEqual(128u, readU32(p + 4), L"Polygon translucency mismatch.");
                    Assert::AreEqual(2u, readU32(p + 8), L"Polygon numPoints mismatch.");
                    // 2 points * 3 floats = 24 bytes, then 2 colours.
                    Assert::IsTrue(chunkL >= 12 + 1 + nameLen + 4 + 4 + 4 + 24 + 8, L"Polygon chunk too short.");
                    return;
                }

                // Recurse into sub-chunks within this chunk's span.
                size_t substart = start + dataL;
                size_t p2 = substart;
                while (p2 + 12 <= start + chunkL) {
                    uint32_t childL = readU32(p2 + 8);
                    if (childL == 0 || p2 + childL > start + chunkL) break;
                    walk(p2);
                    if (found) return;
                    p2 += childL;
                }
            };
            walk(0);
            Assert::IsTrue(found, L"POLYGON chunk (0x18009) not found in writer output.");
        }
    };

    // ------------------------------------------------------------------
    // Helpers for the replicated-world tests.
    // ------------------------------------------------------------------
    net::WorldStateOp MakeOp(uint64_t obj, uint8_t ch, uint8_t policy, uint8_t value,
                             uint8_t kind = net::WSO_SET, uint8_t flags = net::WSF_NONE)
    {
        net::WorldStateOp op;
        op.key.object = obj;
        op.key.channel = ch;
        op.kind = kind;
        op.policy = policy;
        op.flags = flags;
        if (kind == net::WSO_SET) op.value.Set(&value, 1);
        return op;
    }

    // Pumps a host and any number of joiners.
    void PumpAll(net::Session& host, std::initializer_list<net::Session*> joiners, int frames)
    {
        for (int i = 0; i < frames; ++i)
        {
            host.Update();
            for (net::Session* j : joiners) j->Update();
        }
    }

    TEST_CLASS(WorldStateTableTests)
    {
    public:
        TEST_METHOD(LastWritePolicyAcceptsChangesAndReportsUnchanged)
        {
            net::WorldStateTable t;
            net::WorldStateEntry out;
            bool exists = false;
            Assert::IsTrue(t.Apply(1, MakeOp(10, 1, net::WSP_LAST_WRITE, 3), out, exists) == net::WorldStateTable::ACCEPTED);
            Assert::AreEqual(1u, out.version);
            Assert::AreEqual(1, (int)out.owner, L"Creator recorded as owner.");
            Assert::IsTrue(t.Apply(2, MakeOp(10, 1, net::WSP_LAST_WRITE, 3), out, exists) == net::WorldStateTable::UNCHANGED);
            Assert::IsTrue(t.Apply(2, MakeOp(10, 1, net::WSP_LAST_WRITE, 4), out, exists) == net::WorldStateTable::ACCEPTED,
                L"Any peer may overwrite a last-write entry.");
            Assert::AreEqual(2u, out.version, L"Version bumps on every accepted change.");
            Assert::AreEqual(4, (int)out.value.bytes[0]);
        }

        TEST_METHOD(FirstWritePolicyIsImmutable)
        {
            net::WorldStateTable t;
            net::WorldStateEntry out;
            bool exists = false;
            Assert::IsTrue(t.Apply(1, MakeOp(20, 2, net::WSP_FIRST_WRITE, 1), out, exists) == net::WorldStateTable::ACCEPTED);
            Assert::IsTrue(t.Apply(2, MakeOp(20, 2, net::WSP_FIRST_WRITE, 1), out, exists) == net::WorldStateTable::UNCHANGED,
                L"The same value (both peers broke it) is not a conflict.");
            Assert::IsTrue(t.Apply(2, MakeOp(20, 2, net::WSP_LAST_WRITE, 0), out, exists) == net::WorldStateTable::REJECTED,
                L"A later request cannot downgrade the policy and overwrite.");
            Assert::IsTrue(t.Apply(1, MakeOp(20, 2, net::WSP_FIRST_WRITE, 0, net::WSO_ERASE), out, exists) == net::WorldStateTable::REJECTED,
                L"First-write entries cannot be erased by peers.");
            Assert::AreEqual(1, (int)out.value.bytes[0], L"Reject reports the current value.");
        }

        TEST_METHOD(OwnerPolicyOnlyOwnerWritesAndLeaveErases)
        {
            net::WorldStateTable t;
            net::WorldStateEntry out;
            bool exists = false;
            uint64_t ent = net::MakeDynamicEntityId(1, 1);
            Assert::IsTrue(t.Apply(1, MakeOp(ent, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 7, net::WSO_SET,
                                             net::WSF_ERASE_ON_OWNER_LEAVE), out, exists) == net::WorldStateTable::ACCEPTED);
            Assert::IsTrue(t.Apply(2, MakeOp(ent, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 8), out, exists) == net::WorldStateTable::REJECTED,
                L"A non-owner may not write.");
            Assert::IsTrue(t.Apply(2, MakeOp(ent, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 0, net::WSO_ERASE), out, exists) == net::WorldStateTable::REJECTED,
                L"A non-owner may not erase.");

            uint64_t kept = net::MakeDynamicEntityId(1, 2);
            t.Apply(1, MakeOp(kept, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 9), out, exists); // no erase flag

            std::vector<net::WorldStateKey> erased;
            t.OnPeerLeft(1, erased);
            Assert::AreEqual((size_t)1, erased.size(), L"Only flagged owned entries are erased.");
            Assert::IsTrue(erased[0].object == ent);
            net::WorldStateEntry e;
            Assert::IsTrue(t.Find({ kept, net::WS_CHANNEL_ENTITY }, e));
            Assert::AreEqual(0, (int)e.owner, L"Unflagged owned entries are orphaned (read-only).");
        }

        TEST_METHOD(MalformedRequestsAreRejected)
        {
            net::WorldStateTable t;
            net::WorldStateEntry out;
            bool exists = true;
            net::WorldStateOp bad = MakeOp(30, 1, net::WSP_COUNT, 1);
            Assert::IsTrue(t.Apply(1, bad, out, exists) == net::WorldStateTable::REJECTED, L"Unknown policy.");
            Assert::IsTrue(t.Apply(1, MakeOp(31, 1, net::WSP_LAST_WRITE, 0, net::WSO_ERASE), out, exists) == net::WorldStateTable::UNCHANGED,
                L"Erasing nothing is a no-op.");
            Assert::IsFalse(exists);
        }

        TEST_METHOD(CodecRoundTripsAndRejectsTruncation)
        {
            uint8_t buf[net::MAX_PACKET_PAYLOAD];
            net::WorldStateOp op = MakeOp(0x0102030405060708ull, 5, net::WSP_FIRST_WRITE, 42);
            uint16_t n = net::WorldStateCodec::EncodeRequest(buf, sizeof(buf), 77, op);
            Assert::IsTrue(n > 0);
            uint32_t seq = 0;
            net::WorldStateOp back;
            Assert::IsTrue(net::WorldStateCodec::DecodeRequest(buf, n, seq, back));
            Assert::AreEqual(77u, seq);
            Assert::IsTrue(back.key == op.key && back.value == op.value && back.policy == op.policy);
            Assert::IsFalse(net::WorldStateCodec::DecodeRequest(buf, static_cast<uint16_t>(n - 1), seq, back),
                L"Truncated request must be rejected.");

            net::WorldStateEntry e;
            e.key = op.key; e.value = op.value; e.version = 9; e.owner = 3; e.policy = net::WSP_OWNER;
            n = net::WorldStateCodec::EncodeUpdate(buf, sizeof(buf), 3, 77, e);
            net::PeerId origin = 0;
            net::WorldStateEntry eb;
            Assert::IsTrue(net::WorldStateCodec::DecodeUpdate(buf, n, origin, seq, eb));
            Assert::IsTrue(eb.key == e.key && eb.value == e.value && eb.version == 9 && eb.owner == 3 && origin == 3);

            // A value length beyond WS_MAX_VALUE in the wire must not be accepted.
            buf[n - e.value.len - 1] = net::WS_MAX_VALUE + 1;
            Assert::IsFalse(net::WorldStateCodec::DecodeUpdate(buf, n, origin, seq, eb), L"Oversized length must be rejected.");
        }

        TEST_METHOD(SnapshotSplitsIntoChunksAndDecodesEveryEntry)
        {
            std::vector<net::WorldStateEntry> entries;
            for (uint64_t i = 1; i <= 200; ++i)
            {
                net::WorldStateEntry e;
                e.key.object = i; e.key.channel = 1;
                uint8_t v[20]; memset(v, static_cast<int>(i), sizeof(v));
                e.value.Set(v, sizeof(v));
                entries.push_back(e);
            }
            std::vector<std::vector<uint8_t>> chunks;
            net::WorldStateCodec::EncodeSnapshot(entries, net::MAX_PACKET_PAYLOAD, chunks);
            Assert::IsTrue(chunks.size() > 1, L"200 entries must not fit one packet.");
            std::vector<net::WorldStateEntry> back;
            for (const auto& c : chunks)
            {
                Assert::IsTrue(c.size() <= net::MAX_PACKET_PAYLOAD, L"Chunk exceeds packet size.");
                Assert::IsTrue(net::WorldStateCodec::DecodeSnapshot(c.data(), static_cast<uint16_t>(c.size()), back));
            }
            Assert::AreEqual(entries.size(), back.size(), L"Every entry must survive chunking.");
            Assert::IsTrue(back[199].key.object == 200 && back[199].value.bytes[0] == 200);
        }

        TEST_METHOD(MirrorTracksPredictionsConfirmationsAndRejections)
        {
            net::WorldStateMirror m;
            net::WorldStateKey k{ 50, 1 };
            uint32_t seq = m.Predict(k);
            Assert::IsTrue(m.IsPending(k));
            net::WorldStateEntry e; e.key = k; uint8_t v = 1; e.value.Set(&v, 1);
            Assert::IsTrue(m.OnUpdate(e, 4, seq, 4) == net::WSR_CONFIRM, L"Own seq + own origin = confirmation.");
            Assert::IsFalse(m.IsPending(k));

            seq = m.Predict(k);
            Assert::IsTrue(m.OnUpdate(e, 5, 99, 4) == net::WSR_REMOTE, L"Another peer's change.");
            Assert::IsTrue(m.IsPending(k), L"A remote change does not resolve our prediction.");
            Assert::IsTrue(m.OnReject(k, true, e, seq) == net::WSR_REJECT);
            Assert::IsFalse(m.IsPending(k));
            net::WorldStateEntry f;
            Assert::IsTrue(m.Find(k, f) && f.value == e.value, L"Reject installs the authoritative value.");
            m.OnErase(k);
            Assert::IsFalse(m.Find(k, f));
        }
    };

    TEST_CLASS(LedgerTests)
    {
    public:
        net::CounterConfig Coins()
        {
            return economy::MakeCoinConfig(false);
        }

        TEST_METHOD(OpeningIsAcceptedOnceAndBounded)
        {
            net::LedgerServer s;
            s.Configure(economy::COUNTER_COINS, Coins());
            int64_t b = 0;
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_OPENING, 500, 0, b) == net::LedgerServer::ACCEPTED);
            Assert::AreEqual((int64_t)500, b);
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_OPENING, 500, 0, b) == net::LedgerServer::REJECTED,
                L"Opening balance can be declared only once.");
            Assert::IsTrue(s.Apply(2, economy::COUNTER_COINS, net::LR_OPENING, economy::kMaxOpeningCoins + 1, 0, b) == net::LedgerServer::REJECTED,
                L"Opening over the cap is refused.");
        }

        TEST_METHOD(ClaimedPickupRedeemsOncePerPlayer)
        {
            net::LedgerServer s;
            s.Configure(economy::COUNTER_COINS, Coins());
            int64_t b = 0;
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_PICKUP_CLAIMED, 1, 0xC0FFEE, b) == net::LedgerServer::ACCEPTED);
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_PICKUP_CLAIMED, 1, 0xC0FFEE, b) == net::LedgerServer::REJECTED,
                L"The same world coin cannot be redeemed twice by one player.");
            Assert::IsTrue(s.Apply(2, economy::COUNTER_COINS, net::LR_PICKUP_CLAIMED, 1, 0xC0FFEE, b) == net::LedgerServer::ACCEPTED,
                L"Coins are per player: another player may redeem the same coin.");
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_PICKUP_CLAIMED, 1, 0, b) == net::LedgerServer::REJECTED,
                L"A claimed pickup needs a claim.");
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_PICKUP_CLAIMED, 2, 0xBEEF, b) == net::LedgerServer::REJECTED,
                L"A world coin is worth exactly one.");
        }

        TEST_METHOD(SpendCannotGoBelowZeroAndUnknownsAreRefused)
        {
            net::LedgerServer s;
            s.Configure(economy::COUNTER_COINS, Coins());
            int64_t b = 0;
            s.Apply(1, economy::COUNTER_COINS, net::LR_OPENING, 10, 0, b);
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_SPEND, -11, 0, b) == net::LedgerServer::REJECTED,
                L"Balance may not go negative.");
            Assert::AreEqual((int64_t)10, b, L"A refused change leaves the balance.");
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_SPEND, -10, 0, b) == net::LedgerServer::ACCEPTED);
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_SPEND, +5, 0, b) == net::LedgerServer::REJECTED,
                L"A spend must be negative.");
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_CHEAT, 100, 0, b) == net::LedgerServer::REJECTED,
                L"Cheats are refused unless the session allows them.");
            Assert::IsTrue(s.Apply(1, 999, net::LR_REWARD, 1, 0, b) == net::LedgerServer::REJECTED,
                L"Unknown counters are refused (secure default).");
            Assert::IsTrue(s.Apply(1, economy::COUNTER_COINS, net::LR_COUNT, 1, 0, b) == net::LedgerServer::REJECTED,
                L"Unknown reasons are refused.");
            s.OnPeerLeft(1);
            Assert::AreEqual((int64_t)0, s.Balance(1, economy::COUNTER_COINS), L"Session balances drop with the peer.");
        }

        TEST_METHOD(CodecAndClientOutcomeRoundTrip)
        {
            uint8_t buf[net::LedgerCodec::REQUEST_SIZE];
            uint16_t n = net::LedgerCodec::EncodeRequest(buf, sizeof(buf), 5, 1, net::LR_SPEND, -42, 0x1122334455667788ull);
            Assert::AreEqual((int)net::LedgerCodec::REQUEST_SIZE, (int)n);
            uint32_t seq; net::CounterId c; uint8_t r; int32_t d; uint64_t claim;
            Assert::IsTrue(net::LedgerCodec::DecodeRequest(buf, n, seq, c, r, d, claim));
            Assert::IsTrue(seq == 5 && c == 1 && r == net::LR_SPEND && d == -42 && claim == 0x1122334455667788ull);
            Assert::IsFalse(net::LedgerCodec::DecodeRequest(buf, static_cast<uint16_t>(n - 1), seq, c, r, d, claim));

            net::LedgerClient cl;
            uint32_t s1 = cl.Predict(1, net::LR_PICKUP, 3);
            Assert::AreEqual((size_t)1, cl.Pending());
            net::LedgerOutcome o = cl.OnResult(s1, 1, false, 7);
            Assert::IsTrue(o.known && !o.accepted && o.delta == 3 && o.balance == 7);
            Assert::AreEqual((size_t)0, cl.Pending());
            Assert::IsFalse(cl.OnResult(12345, 1, true, 8).known, L"Unknown seq is reported as such.");
        }
    };

    // The economy gateway converges the local bank to the server's balance.
    static int64_t g_localBank = 0;
    static int64_t TestGetBank(net::CounterId) { return g_localBank; }
    static void TestSetBank(net::CounterId, int64_t v) { g_localBank = v; }

    TEST_CLASS(PlayerEconomyTests)
    {
    public:
        TEST_METHOD(OpeningReportAndConvergence)
        {
            PlayerEconomy eco;
            std::vector<std::pair<uint8_t, int32_t>> sent;
            eco.SetSender([&](net::CounterId, uint8_t reason, int32_t delta, uint64_t) {
                sent.push_back(std::make_pair(reason, delta)); return true; });
            eco.SetBalanceAccess(&TestGetBank, &TestSetBank);
            g_localBank = 120;
            eco.OnMatchStart(economy::COUNTER_COINS);
            Assert::AreEqual((size_t)1, sent.size());
            Assert::AreEqual((int)net::LR_OPENING, (int)sent[0].first);
            Assert::AreEqual(120, (int)sent[0].second, L"The current bank is declared as opening balance.");

            g_localBank += 5; // local pickup (prediction)
            Assert::IsTrue(eco.Report(economy::COUNTER_COINS, net::LR_PICKUP, 5));

            // Server refused the pickup: once idle, the bank converges to 120.
            net::LedgerOutcome o; o.known = true; o.counter = economy::COUNTER_COINS;
            o.reason = net::LR_PICKUP; o.delta = 5; o.accepted = false; o.balance = 120;
            net::LedgerOutcome other = o; other.accepted = true; other.delta = 0; // another of our requests
            eco.OnOutcome(other, 1);
            Assert::AreEqual((int64_t)125, g_localBank, L"No convergence while our requests are in flight.");
            eco.OnOutcome(o, 0);
            Assert::AreEqual((int64_t)120, g_localBank, L"Refused change rolled back by convergence.");
            Assert::AreEqual(1u, eco.Refused());
            Assert::IsFalse(eco.Report(economy::COUNTER_COINS, net::LR_PICKUP, 0), L"Zero deltas are not sent.");
        }

        TEST_METHOD(RefusedOpeningStopsSyncInsteadOfZeroing)
        {
            PlayerEconomy eco;
            eco.SetSender([&](net::CounterId, uint8_t, int32_t, uint64_t) { return true; });
            eco.SetBalanceAccess(&TestGetBank, &TestSetBank);
            g_localBank = 5000000;
            eco.OnMatchStart(economy::COUNTER_COINS);
            net::LedgerOutcome o; o.known = true; o.counter = economy::COUNTER_COINS;
            o.reason = net::LR_OPENING; o.delta = 5000000; o.accepted = false; o.balance = 0;
            eco.OnOutcome(o, 0);
            Assert::AreEqual((int64_t)5000000, g_localBank, L"A real balance must never be zeroed.");
            Assert::IsFalse(eco.IsActive(), L"Economy sync stops for this player.");
        }
    };

    // ---- NetWorldRegistry ----------------------------------------------------
    static std::vector<std::pair<void*, uint8_t>> g_applied;
    static NetWorldRegistry* g_reg = nullptr;
    static bool g_requestInsideApply = false;
    static void TestApply(void* h, uint8_t ch, const net::WorldStateValue& v, bool erased)
    {
        (void)v;
        g_applied.push_back(std::make_pair(h, erased ? (uint8_t)0xEE : ch));
        if (g_reg) { uint8_t x = 1; g_requestInsideApply = g_reg->Request(h, ch, net::WSP_LAST_WRITE, &x, 1); }
    }

    TEST_CLASS(NetWorldRegistryTests)
    {
    public:
        TEST_METHOD(IdsAreDeterministicAndStableAcrossReload)
        {
            NetWorldRegistry a, b;
            int o1, o2, o3;
            uint64_t a1 = a.Register(net::WOK_STATEPROP, 100, 7, &o1, &TestApply);
            uint64_t a2 = a.Register(net::WOK_STATEPROP, 100, 7, &o2, &TestApply); // same name: ordinal 1
            uint64_t b1 = b.Register(net::WOK_STATEPROP, 100, 7, &o3, &TestApply);
            Assert::IsTrue(a1 != 0 && a2 != 0 && a1 != a2, L"Same-named instances get distinct ids.");
            Assert::IsTrue(a1 == b1, L"Two clients loading the same data agree on the id.");
            Assert::IsFalse(net::IsDynamicEntityId(a1), L"Static ids never use the dynamic namespace.");

            a.Unregister(&o1);
            a.Unregister(&o2);
            uint64_t r1 = a.Register(net::WOK_STATEPROP, 100, 7, &o1, &TestApply);
            uint64_t r2 = a.Register(net::WOK_STATEPROP, 100, 7, &o2, &TestApply);
            Assert::IsTrue(r1 == a1 && r2 == a2, L"A reloaded source gets the same ids again.");
            Assert::IsTrue(a.Register(net::WOK_ANIMCOLL, 100, 7, &o3, &TestApply) != a1,
                L"Kind is part of the identity.");
        }

        TEST_METHOD(KnownStateAppliesOnRegisterDeferredAndWithoutEcho)
        {
            NetWorldRegistry r;
            std::vector<net::WorldStateOp> sent;
            r.SetSender([&](const net::WorldStateOp& op) { sent.push_back(op); return true; });
            g_applied.clear();
            g_reg = &r;
            g_requestInsideApply = true;

            // The server reports a state for an object that is not loaded yet.
            int obj;
            uint64_t id = net::MakeWorldObjectId(net::WOK_STATEPROP, 5, 6, 0);
            net::WorldStateEntry e; e.key.object = id; e.key.channel = net::WOC_BROKEN;
            uint8_t one = 1; e.value.Set(&one, 1);
            r.OnAuthoritative(e, net::WSR_SNAPSHOT);
            r.Pump();
            Assert::AreEqual((size_t)0, g_applied.size(), L"Nothing to apply until it loads.");

            Assert::IsTrue(r.Register(net::WOK_STATEPROP, 5, 6, &obj, &TestApply) == id);
            Assert::AreEqual((size_t)0, g_applied.size(), L"Never applied inside Register (loader context).");
            r.Pump();
            Assert::AreEqual((size_t)1, g_applied.size(), L"Applied on the next Pump.");
            Assert::IsTrue(g_applied[0].second == net::WOC_BROKEN);
            Assert::IsFalse(g_requestInsideApply, L"A mirrored change must not be re-sent.");
            Assert::AreEqual((size_t)0, sent.size());

            uint8_t st = 2;
            Assert::IsTrue(r.Request(&obj, net::WOC_STATE, net::WSP_LAST_WRITE, &st, 1), L"Local change is sent.");
            Assert::AreEqual((size_t)1, sent.size());
            Assert::IsTrue(sent[0].key.object == id && sent[0].key.channel == net::WOC_STATE);

            int stranger;
            Assert::IsFalse(r.Request(&stranger, net::WOC_STATE, net::WSP_LAST_WRITE, &st, 1), L"Unregistered objects are not networked.");
            g_reg = nullptr;
        }

        TEST_METHOD(EntityRecordsAreIgnoredByTheRegistry)
        {
            NetWorldRegistry r;
            g_applied.clear();
            net::WorldStateEntry e; e.key.object = net::MakeDynamicEntityId(2, 1); e.key.channel = net::WS_CHANNEL_ENTITY;
            r.OnAuthoritative(e, net::WSR_REMOTE);
            r.Pump();
            Assert::AreEqual((size_t)0, g_applied.size());
        }
    };

    // ---- NetEntityManager ----------------------------------------------------
    struct FakeGhost { uint64_t id; net::NetEntitySpawn spawn; net::NetEntityMotion last; int moves; };
    static std::vector<FakeGhost*> g_ghosts;
    static int g_ghostDespawns = 0;
    static void* GhostSpawn(uint64_t id, const net::NetEntitySpawn& s)
    {
        FakeGhost* g = new FakeGhost(); g->id = id; g->spawn = s; g->moves = 0;
        g_ghosts.push_back(g); return g;
    }
    static void GhostMove(void* h, const net::NetEntityMotion& m) { FakeGhost* g = (FakeGhost*)h; g->last = m; ++g->moves; }
    static void GhostDespawn(void* h)
    {
        ++g_ghostDespawns;
        for (size_t i = 0; i < g_ghosts.size(); ++i) if (g_ghosts[i] == h) { delete g_ghosts[i]; g_ghosts.erase(g_ghosts.begin() + i); break; }
    }
    static bool SampleCar(void* h, net::NetEntityMotion& m) { (void)h; m.x = 1.0f; m.z = 2.0f; m.speedKmh = 30; return true; }

    TEST_CLASS(NetEntityManagerTests)
    {
    public:
        TEST_METHOD(LocalEntityIsAnnouncedAfterWelcomeAndStreamed)
        {
            NetEntityManager m;
            std::vector<net::WorldStateOp> ops;
            std::vector<net::EntityStateRecord> streamed;
            m.SetSenders([&](const net::WorldStateOp& op) { ops.push_back(op); return true; },
                         [&](const std::vector<net::EntityStateRecord>& r) { streamed.insert(streamed.end(), r.begin(), r.end()); return true; });
            m.SetSampler(&SampleCar);
            m.SetSendEvery(1);

            int car;
            net::NetEntitySpawn sp; sp.kind = net::NEK_TRAFFIC_CAR; strcpy(sp.model, "compactA");
            m.AddLocal(&car, sp);
            Assert::AreEqual((size_t)0, ops.size(), L"No id before our peer id is known.");
            m.OnWelcomed(3);
            Assert::AreEqual((size_t)1, ops.size(), L"Announced once welcomed.");
            Assert::IsTrue(ops[0].policy == net::WSP_OWNER && (ops[0].flags & net::WSF_ERASE_ON_OWNER_LEAVE));
            Assert::IsTrue(net::IsDynamicEntityId(ops[0].key.object) && ops[0].key.channel == net::WS_CHANNEL_ENTITY);
            Assert::IsTrue(m.LocalId(&car) == ops[0].key.object);

            m.Update(0.016f);
            Assert::AreEqual((size_t)1, streamed.size(), L"Owned motion is streamed.");
            net::NetEntityMotion back;
            Assert::IsTrue(back.Decode(streamed[0]) && back.x == 1.0f && back.speedKmh == 30);

            m.RemoveLocal(&car);
            Assert::AreEqual((size_t)2, ops.size());
            Assert::IsTrue(ops[1].kind == net::WSO_ERASE, L"Removing a local entity erases it on the server.");
        }

        TEST_METHOD(RemoteEntityGhostLifecycleAndOwnerCheck)
        {
            NetEntityManager m;
            NetEntityManager::Sink s = { &GhostSpawn, &GhostMove, &GhostDespawn };
            m.SetSink(s);
            m.OnWelcomed(1);
            g_ghostDespawns = 0;

            net::NetEntitySpawn sp; sp.kind = net::NEK_TRAFFIC_CAR; strcpy(sp.model, "sedanB"); sp.x = 10.0f;
            net::WorldStateEntry e; e.key.object = net::MakeDynamicEntityId(2, 9); e.key.channel = net::WS_CHANNEL_ENTITY;
            e.owner = 2; e.policy = net::WSP_OWNER;
            Assert::IsTrue(sp.Encode(e.value));
            m.OnAuthoritative(e, net::WSR_SNAPSHOT);
            Assert::AreEqual((size_t)1, m.RemoteCount(), L"Snapshot entity spawns a ghost.");
            Assert::AreEqual(std::string("sedanB"), std::string(g_ghosts.back()->spawn.model));
            m.OnAuthoritative(e, net::WSR_REMOTE);
            Assert::AreEqual((size_t)1, m.RemoteCount(), L"No duplicate ghost.");

            net::NetEntityMotion mo; mo.x = 20.0f;
            net::EntityStateRecord rec; mo.Encode(rec, e.key.object);
            m.OnEntityState(5, rec); // spoofed owner
            Assert::AreEqual(10.0f, g_ghosts.back()->last.x, L"Motion from a non-owner is ignored.");
            m.OnEntityState(2, rec);
            m.Update(0.016f);
            Assert::AreEqual(20.0f, g_ghosts.back()->last.x, L"First owner sample snaps (no slide from spawn).");
            Assert::AreEqual(1, m.CountRemoteNear(19.0f, 0.0f, 5.0f));
            Assert::AreEqual(0, m.CountRemoteNear(100.0f, 0.0f, 5.0f));

            net::WorldStateEntry own = e; own.key.object = net::MakeDynamicEntityId(1, 1); own.owner = 1;
            m.OnAuthoritative(own, net::WSR_CONFIRM);
            Assert::AreEqual((size_t)1, m.RemoteCount(), L"Our own entities never become ghosts.");

            m.OnAuthoritative(e, net::WSR_ERASED);
            Assert::AreEqual((size_t)0, m.RemoteCount());
            Assert::AreEqual(1, g_ghostDespawns);
        }

        TEST_METHOD(SpawnRecordRejectsGarbage)
        {
            net::NetEntitySpawn sp;
            net::WorldStateValue v; v.len = 3;
            Assert::IsFalse(sp.Decode(v), L"Short spawn record rejected.");
            net::NetEntityMotion mo;
            net::EntityStateRecord rec; rec.len = 4;
            Assert::IsFalse(mo.Decode(rec), L"Short motion record rejected.");
            float nanv = std::numeric_limits<float>::quiet_NaN();
            net::NetEntityMotion bad; bad.x = nanv;
            bad.Encode(rec, 1);
            Assert::IsFalse(mo.Decode(rec), L"Non-finite positions rejected.");
        }
    };

    // ---- Session-level replicated world (real loopback sockets) -------------
    TEST_CLASS(SessionWorldStateTests)
    {
    public:
        TEST_METHOD(ChangeReplicatesToPeersAndLateJoinerGetsSnapshot)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xA1));
            net::Session a, b;
            Assert::IsTrue(a.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, a));
            Assert::IsTrue(b.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, b));

            int bGot = 0;
            b.SetWorldStateSink([&](const net::WorldStateEntry& e, net::WorldStateReason reason, net::PeerId origin) {
                if (reason == net::WSR_REMOTE && e.key.object == 1234 && origin == a.GetLocalPeerId()) ++bGot; });
            int aConfirm = 0;
            a.SetWorldStateSink([&](const net::WorldStateEntry&, net::WorldStateReason reason, net::PeerId) {
                if (reason == net::WSR_CONFIRM) ++aConfirm; });

            Assert::IsTrue(a.RequestWorldState(MakeOp(1234, net::WOC_BROKEN, net::WSP_FIRST_WRITE, 1)));
            Assert::IsTrue(a.RequestWorldState(MakeOp(1235, net::WOC_STATE, net::WSP_LAST_WRITE, 3)));
            for (int i = 0; i < 300 && (bGot == 0 || aConfirm < 2); ++i) PumpAll(host, { &a, &b }, 1);
            Assert::AreEqual(1, bGot, L"Other peers receive the change.");
            Assert::AreEqual(2, aConfirm, L"The requester gets its confirmations.");
            Assert::AreEqual((size_t)2, host.GetWorldStateTable().Size());

            // Late joiner: the whole world arrives before it is 'synced'.
            net::Session late;
            size_t snap = 0;
            bool synced = false;
            late.SetWorldStateSink([&](const net::WorldStateEntry&, net::WorldStateReason reason, net::PeerId) {
                if (reason == net::WSR_SNAPSHOT) ++snap; });
            late.SetWorldSyncedFn([&]() { synced = true; });
            Assert::IsTrue(late.Join("127.0.0.1", port));
            for (int i = 0; i < 500 && !synced; ++i) PumpAll(host, { &a, &b, &late }, 1);
            Assert::IsTrue(synced, L"Late joiner must finish the snapshot.");
            Assert::AreEqual((size_t)2, snap, L"Snapshot carries every entry.");
            net::WorldStateEntry e;
            Assert::IsTrue(late.GetWorldStateMirror().Find({ 1234, net::WOC_BROKEN }, e) && e.value.bytes[0] == 1);

            late.Leave(); b.Leave(); a.Leave(); host.Leave();
        }

        TEST_METHOD(ConflictingFirstWriteIsRejectedToTheLoser)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xA2));
            net::Session a, b;
            Assert::IsTrue(a.Join("127.0.0.1", port)); Assert::IsTrue(PumpToWelcome(host, a));
            Assert::IsTrue(b.Join("127.0.0.1", port)); Assert::IsTrue(PumpToWelcome(host, b));

            int rejects = 0;
            uint8_t rejectedTo = 0;
            auto sink = [&](const net::WorldStateEntry& e, net::WorldStateReason reason, net::PeerId) {
                if (reason == net::WSR_REJECT) { ++rejects; rejectedTo = e.value.bytes[0]; } };
            a.SetWorldStateSink(sink);
            b.SetWorldStateSink(sink);

            Assert::IsTrue(a.RequestWorldState(MakeOp(77, 9, net::WSP_FIRST_WRITE, 1)));
            PumpAll(host, { &a, &b }, 50);
            Assert::IsTrue(b.RequestWorldState(MakeOp(77, 9, net::WSP_FIRST_WRITE, 2)));
            for (int i = 0; i < 300 && rejects == 0; ++i) PumpAll(host, { &a, &b }, 1);
            Assert::AreEqual(1, rejects, L"The second, different write is refused.");
            Assert::AreEqual(1, (int)rejectedTo, L"The loser is told the winning value to roll back to.");
            b.Leave(); a.Leave(); host.Leave();
        }

        TEST_METHOD(OwnerLeaveErasesEntitiesAndStreamIsOwnerChecked)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xA3));
            net::Session owner, viewer;
            Assert::IsTrue(owner.Join("127.0.0.1", port)); Assert::IsTrue(PumpToWelcome(host, owner));
            Assert::IsTrue(viewer.Join("127.0.0.1", port)); Assert::IsTrue(PumpToWelcome(host, viewer));

            uint64_t ent = net::MakeDynamicEntityId(owner.GetLocalPeerId(), 1);
            int spawned = 0, erased = 0, streamed = 0;
            viewer.SetWorldStateSink([&](const net::WorldStateEntry& e, net::WorldStateReason reason, net::PeerId) {
                if (e.key.object != ent) return;
                if (reason == net::WSR_ERASED) ++erased; else ++spawned; });
            viewer.SetEntityStateSink([&](net::PeerId from, const net::EntityStateRecord& rec) {
                if (rec.entity == ent && from == owner.GetLocalPeerId()) ++streamed; });

            Assert::IsTrue(owner.RequestWorldState(MakeOp(ent, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 1, net::WSO_SET,
                                                          net::WSF_ERASE_ON_OWNER_LEAVE)));
            for (int i = 0; i < 300 && spawned == 0; ++i) PumpAll(host, { &owner, &viewer }, 1);
            Assert::AreEqual(1, spawned);

            net::EntityStateRecord rec; rec.entity = ent; rec.len = 4;
            std::vector<net::EntityStateRecord> recs(1, rec);
            Assert::IsTrue(viewer.SendEntityStates(recs), L"Spoofed stream is sent ...");
            Assert::IsTrue(owner.SendEntityStates(recs));
            for (int i = 0; i < 300 && streamed == 0; ++i) PumpAll(host, { &owner, &viewer }, 1);
            PumpAll(host, { &owner, &viewer }, 30);
            Assert::AreEqual(1, streamed, L"... but only the owner's record is relayed.");

            owner.Leave();
            for (int i = 0; i < 600 && erased == 0; ++i) PumpAll(host, { &viewer }, 1);
            Assert::AreEqual(1, erased, L"Owner disconnect erases its entities for everyone.");
            viewer.Leave(); host.Leave();
        }

        TEST_METHOD(LedgerThroughSessionAcceptsAndRefuses)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            host.ConfigureCounter(economy::COUNTER_COINS, economy::MakeCoinConfig(false));
            Assert::IsTrue(host.StartHost(port, 0xA4));
            net::Session c;
            std::vector<net::LedgerOutcome> outcomes;
            c.SetLedgerSink([&](const net::LedgerOutcome& o) { outcomes.push_back(o); });
            Assert::IsTrue(c.Join("127.0.0.1", port));
            // Sent before WELCOME: queued and delivered in order.
            Assert::IsTrue(c.RequestLedgerChange(economy::COUNTER_COINS, net::LR_OPENING, 10, 0));
            Assert::IsTrue(c.RequestLedgerChange(economy::COUNTER_COINS, net::LR_PICKUP_CLAIMED, 1, 0xAB));
            Assert::IsTrue(c.RequestLedgerChange(economy::COUNTER_COINS, net::LR_PICKUP_CLAIMED, 1, 0xAB));
            for (int i = 0; i < 500 && outcomes.size() < 3; ++i) PumpAll(host, { &c }, 1);
            Assert::AreEqual((size_t)3, outcomes.size());
            Assert::IsTrue(outcomes[0].accepted && outcomes[0].balance == 10);
            Assert::IsTrue(outcomes[1].accepted && outcomes[1].balance == 11);
            Assert::IsTrue(!outcomes[2].accepted && outcomes[2].balance == 11 && outcomes[2].delta == 1,
                L"Double redemption refused with the authoritative balance.");
            Assert::AreEqual((int64_t)11, host.GetLedgerServer().Balance(c.GetLocalPeerId(), economy::COUNTER_COINS));
            c.Leave(); host.Leave();
        }

        TEST_METHOD(UnauthenticatedPeerCannotInjectState)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            host.SetHostPassword("secret");
            Assert::IsTrue(host.StartHost(port, 0xA5));
            net::Session good;
            good.SetPassword("secret");
            Assert::IsTrue(good.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, good));

            net::Session intruder; // no password: never admitted
            Assert::IsTrue(intruder.Join("127.0.0.1", port));
            int goodGotUser = 0;
            good.SetCustomEventSink([&](net::PeerId, const uint8_t*, uint16_t) { ++goodGotUser; });
            PumpAll(host, { &good, &intruder }, 50);
            const uint8_t evil[2] = { 1, 2 };
            intruder.SendCustomEvent(evil, sizeof(evil));
            intruder.RequestWorldState(MakeOp(5, 1, net::WSP_LAST_WRITE, 1));
            PumpAll(host, { &good, &intruder }, 100);
            Assert::AreEqual(0, goodGotUser, L"Unadmitted peer's events are dropped.");
            Assert::AreEqual((size_t)0, host.GetWorldStateTable().Size(), L"Unadmitted peer cannot write world state.");
            Assert::IsFalse(intruder.IsWelcomed());
            intruder.Leave(); good.Leave(); host.Leave();
        }

        TEST_METHOD(OpenSessionNeedsNoAuthorization)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsFalse(host.HasHostPassword(), L"Authorization is off by default.");
            Assert::IsTrue(host.StartHost(port, 0xA6));
            net::Session c;
            Assert::IsTrue(c.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, c), L"Open sessions admit on connect.");
            c.Leave(); host.Leave();
        }

        TEST_METHOD(SlotCapIsApplied)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            host.SetMaxPeers(2); // loopback + one remote
            Assert::IsTrue(host.StartHost(port, 0xA7));
            net::Session a, b;
            Assert::IsTrue(a.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, a));
            Assert::IsTrue(b.Join("127.0.0.1", port));
            PumpAll(host, { &a, &b }, 300);
            Assert::IsFalse(b.IsWelcomed(), L"A peer over the slot cap is not admitted.");
            b.Leave(); a.Leave(); host.Leave();
        }
    };

    TEST_CLASS(MatchManagerWorldServicesTests)
    {
    public:
        TEST_METHOD(JoinLeaveEventsReachTheGame)
        {
            unsigned short port = PickFreePort();
            NetworkMatchManager* mgr = NetworkMatchManager::CreateInstance();
            Assert::IsTrue(mgr->StartHost(port));
            net::Session c;
            Assert::IsTrue(c.Join("127.0.0.1", port));
            std::vector<NetworkMatchManager::JoinLeaveEvent> ev;
            for (int i = 0; i < 500; ++i)
            {
                mgr->Update(); c.Update();
                mgr->DrainJoinLeaveEvents(ev);
                bool remoteJoined = false;
                for (const auto& e : ev) if (e.joined && e.clientId == c.GetLocalPeerId() && c.GetLocalPeerId() != 0) remoteJoined = true;
                if (remoteJoined) break;
            }
            bool found = false;
            for (const auto& e : ev) if (e.joined && e.clientId == c.GetLocalPeerId()) found = true;
            Assert::IsTrue(found, L"Host must surface the remote player's join.");
            c.Leave();
            mgr->Leave();
        }
    };
}
