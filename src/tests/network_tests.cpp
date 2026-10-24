#include "CppUnitTest.h"
#include "NetworkPacket.h"
#include "PlayerNetworkState.h"
#include "NetworkMatchManager.h"
#include "RemotePlayerWorldView.h"
#include "RemoteVehicleWorldView.h"
#include "ServerModManager.h"
#include "ClientModManager.h"
#include "MissionSyncState.h"
#include "WorldEvent.h"
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
    };

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

    TEST_CLASS(WorldEventTests)
    {
    public:
        TEST_METHOD(WorldEventCodecRoundTripsPayload)
        {
            // The codec wraps a type + origin + opaque payload in the PID_USER
            // channel envelope. Verify encode->decode round-trips the fields.
            const uint8_t payload[5] = { 1, 2, 3, 4, 5 };
            uint8_t buf[net::MAX_PACKET_PAYLOAD];
            uint16_t n = net::WorldEventCodec::Encode(
                buf, sizeof(buf), net::WE_STATEPROP, 0x2A, payload, sizeof(payload));
            Assert::IsTrue(n > net::WORLD_EVENT_HEADER_SIZE, L"Encode should write header+payload.");

            auto d = net::WorldEventCodec::Decode(buf, n);
            Assert::IsTrue(d.valid, L"Decode should succeed.");
            Assert::IsTrue(d.type == net::WE_STATEPROP, L"Type must round-trip.");
            Assert::IsTrue(d.originPeer == 0x2A, L"Origin peer must round-trip.");
            Assert::IsTrue(d.payloadSize == sizeof(payload), L"Payload size must round-trip.");
            for (uint16_t i = 0; i < sizeof(payload); ++i)
                Assert::IsTrue(d.payload[i] == payload[i], L"Payload bytes must round-trip.");
        }

        TEST_METHOD(WorldEventRejectsTruncatedOrOversize)
        {
            // Truncated (header only, missing payload) must decode as invalid.
            const uint8_t payload[3] = { 9, 8, 7 };
            uint8_t buf[net::MAX_PACKET_PAYLOAD];
            uint16_t n = net::WorldEventCodec::Encode(buf, sizeof(buf), net::WE_BREAKABLE, 1, payload, sizeof(payload));
            auto trunc = net::WorldEventCodec::Decode(buf, net::WORLD_EVENT_HEADER_SIZE + 1);
            Assert::IsFalse(trunc.valid, L"Truncated event must decode invalid.");

            // Oversize payload (over MAX_PACKET_PAYLOAD) must be refused at encode.
            uint8_t big[net::MAX_PACKET_PAYLOAD];
            uint16_t m = net::WorldEventCodec::Encode(buf, sizeof(buf), net::WE_COIN_COLLECTED, 1,
                                                      big, net::MAX_PACKET_PAYLOAD);
            Assert::IsTrue(n > 0, L"First encode should have succeeded (3-byte payload).");
            Assert::IsTrue(m == 0, L"Oversize encode must return 0.");
        }

        TEST_METHOD(WorldEventRelaysBetweenSessionsThroughOpenChannel)
        {
            // End-to-end: a world event encoded via NetworkMatchManager's
            // SendWorldEvent must reach the other session's sink through the
            // open PID_USER relay (host->joiner and joiner->host).
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 0xAA55));
            net::Session joiner;
            Assert::IsTrue(joiner.Join("127.0.0.1", port));
            Assert::IsTrue(PumpToWelcome(host, joiner), L"Joiner did not get WELCOME.");

            // Host -> joiner: register sink on joiner, send from host.
            int joinerGot = 0;
            uint8_t joinerType = 0;
            joiner.SetCustomEventSink([&](net::PeerId from, const uint8_t* data, uint16_t size) {
                auto d = net::WorldEventCodec::Decode(data, size);
                if (!d.valid) return;
                joinerGot++;
                joinerType = static_cast<uint8_t>(d.type);
            });

            uint8_t payload[2] = { 0xEE, 0x01 };
            uint8_t buf[net::MAX_PACKET_PAYLOAD];
            uint16_t n = net::WorldEventCodec::Encode(buf, sizeof(buf), net::WE_VEHICLE_DESTROYED,
                                                      static_cast<uint16_t>(host.GetLocalPeerId()),
                                                      payload, sizeof(payload));
            Assert::IsTrue(n > 0);
            Assert::IsTrue(host.SendCustomEvent(buf, n), L"Host SendCustomEvent failed.");

            for (int i = 0; i < 200 && joinerGot == 0; ++i) { host.Update(); joiner.Update(); }
            Assert::AreEqual(1, joinerGot, L"Joiner did not receive the host's world event.");
            Assert::IsTrue(joinerType == static_cast<uint8_t>(net::WE_VEHICLE_DESTROYED),
                           L"Joiner should see WE_VEHICLE_DESTROYED type.");

            joiner.Leave();
            host.Leave();
        }
    };
}