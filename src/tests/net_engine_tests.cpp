// Tests for the transport / server-core layer and the game server rules:
// clock sync, snapshot buffers, rate limits, handshake refusals, embedded and
// dedicated-style servers, movement validation, ownership migration, entity
// and player (PvP) events, interest management and the compact streams.
#include "CppUnitTest.h"
#include "PlayerNetworkState.h"
#include "PlayerMovementValidator.h"
#include "GameServer.h"
#include "NetEntityManager.h"
#include "RemoteStateTrack.h"
#include "PlayerPresence.h"
#include "RemotePlayerWorldView.h"
#include "PlayerInterestPolicy.h"
#include "ObjectClaim.h"
#include "../game/netengine/NetTime.h"
#include "../game/netengine/ServerCore.h"
#include "../game/netengine/Session.h"

#include <winsock2.h>
#include <windows.h>
#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace NetworkTests
{
    // Shared helpers (network_tests.cpp).
    unsigned short PickFreePort();
    void PumpAll(net::Session& host, std::initializer_list<net::Session*> joiners, int frames);
    net::WorldStateOp MakeOp(uint64_t obj, uint8_t ch, uint8_t policy, uint8_t value, uint8_t kind, uint8_t flags);

    namespace
    {
        // Pumps until `done` or the frame budget runs out (1 ms per frame, so
        // time-based cadences progress).
        template <typename Done>
        bool PumpUntil(std::initializer_list<net::Session*> sessions, Done done, int frames = 2000)
        {
            for (int i = 0; i < frames; ++i)
            {
                for (net::Session* s : sessions) s->Update();
                if (done()) return true;
                Sleep(1);
            }
            return false;
        }

        PlayerNetworkState StateAt(float x, float z, uint8_t teleportSeq = 0)
        {
            PlayerNetworkState st;
            std::memset(&st, 0, sizeof(st));
            st.x = x; st.z = z;
            st.teleportSeq = teleportSeq;
            return st;
        }

        std::vector<uint8_t> Blob(const PlayerNetworkState& st)
        {
            std::vector<uint8_t> b(PlayerNetworkState::MAX_SERIALIZED_SIZE);
            b.resize(static_cast<size_t>(st.Serialize(reinterpret_cast<char*>(b.data()))));
            return b;
        }
    }

    TEST_CLASS(NetTimeTests)
    {
    public:
        TEST_METHOD(ClockSyncUsesTheLowestRttSample)
        {
            net::ClockSync c;
            // Server is 5000 ms ahead. Sample 1: rtt 100 (server stamped at +50).
            c.OnPong(1000, 6050, 1100);
            Assert::AreEqual((uint32_t)6100, c.ServerTime(1100));
            // A queued sample: rtt 300, stamped late (+250): offset estimate off.
            c.OnPong(2000, 7250, 2300);
            Assert::AreEqual((uint32_t)7300, c.ServerTime(2300), L"Still the 100 ms sample's offset.");
            // A better sample: rtt 20.
            c.OnPong(3000, 8010, 3020);
            Assert::AreEqual((uint32_t)8020, c.ServerTime(3020));
            Assert::IsTrue(c.RttMs() > 0 && c.RttMs() < 300);
        }

        TEST_METHOD(SnapshotBufferBracketsAndExtrapolates)
        {
            net::SnapshotBuffer<float> b;
            b.Push(100, 0.0f);
            b.Push(300, 20.0f);
            b.Push(200, 10.0f); // out of order: inserted in place
            b.Push(200, 10.0f); // duplicate time: replaced, not added
            Assert::AreEqual((size_t)3, b.Size());
            const float* f = nullptr; const float* t = nullptr; float a = 0.0f;
            Assert::IsTrue(b.Sample(250, 100, f, t, a));
            Assert::AreEqual(10.0f, *f); Assert::AreEqual(20.0f, *t); Assert::AreEqual(0.5f, a, 0.001f);
            Assert::IsTrue(b.Sample(350, 100, f, t, a));
            Assert::AreEqual(1.5f, a, 0.001f, L"Extrapolation past the newest sample.");
            Assert::IsTrue(b.Sample(900, 100, f, t, a));
            Assert::AreEqual(2.0f, a, 0.001f, L"Extrapolation is capped.");
            Assert::IsTrue(b.Sample(50, 100, f, t, a));
            Assert::AreEqual(0.0f, *f, L"Before the history: the oldest sample.");
            b.DiscardBefore(250);
            Assert::AreEqual((size_t)2, b.Size(), L"Keeps one sample older than render time.");
        }

        TEST_METHOD(TokenBucketRefillsAtItsRate)
        {
            net::TokenBucket tb;
            tb.Configure(10.0f, 2.0f);
            Assert::IsTrue(tb.Take(1000));
            Assert::IsTrue(tb.Take(1000));
            Assert::IsFalse(tb.Take(1000), L"Burst spent.");
            Assert::IsTrue(tb.Take(1100), L"100 ms at 10/s refills one token.");
            Assert::IsFalse(tb.Take(1100));
        }
    };

    TEST_CLASS(WorldStateMigrationTests)
    {
    public:
        TEST_METHOD(ReleaseThenFirstClaimWins)
        {
            net::WorldStateTable t;
            net::WorldStateEntry out; bool ex = false;
            net::WorldStateOp set = MakeOp(9, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 1, net::WSO_SET, net::WSF_NONE);
            Assert::IsTrue(t.Apply(1, set, out, ex, 0) == net::WorldStateTable::ACCEPTED);

            net::WorldStateOp claim = MakeOp(9, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 0, net::WSO_CLAIM, 0);
            Assert::IsTrue(t.Apply(2, claim, out, ex, 0) == net::WorldStateTable::REJECTED, L"Not released yet.");

            net::WorldStateOp release = MakeOp(9, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 7, net::WSO_RELEASE, 0);
            const uint8_t pose = 7;
            release.value.Set(&pose, 1); // a release may carry the entity's current value
            Assert::IsTrue(t.Apply(2, release, out, ex, 0) == net::WorldStateTable::REJECTED, L"Only the owner releases.");
            Assert::IsTrue(t.Apply(1, release, out, ex, 100) == net::WorldStateTable::ACCEPTED);
            Assert::IsTrue(out.owner == 0 && (out.flags & net::WSF_CLAIMABLE) && out.value.bytes[0] == 7,
                L"Released with the new value (pose).");

            Assert::IsTrue(t.Apply(2, claim, out, ex, 200) == net::WorldStateTable::ACCEPTED);
            Assert::IsTrue(out.owner == 2 && !(out.flags & net::WSF_CLAIMABLE));
            Assert::IsTrue(t.Apply(3, claim, out, ex, 210) == net::WorldStateTable::REJECTED, L"Second claim loses.");
            Assert::AreEqual(2, (int)out.owner, L"The loser learns the winner.");
        }

        TEST_METHOD(OwnerLeaveReleasesAndUnclaimedExpires)
        {
            net::WorldStateTable t;
            net::WorldStateEntry out; bool ex = false;
            t.Apply(1, MakeOp(1, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 1, net::WSO_SET, net::WSF_RELEASE_ON_OWNER_LEAVE), out, ex, 0);
            t.Apply(1, MakeOp(2, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 1, net::WSO_SET, net::WSF_ERASE_ON_OWNER_LEAVE), out, ex, 0);
            std::vector<net::WorldStateKey> erased;
            std::vector<net::WorldStateEntry> released;
            t.OnPeerLeft(1, erased, &released, 1000);
            Assert::AreEqual((size_t)1, erased.size());
            Assert::AreEqual((size_t)1, released.size());
            Assert::IsTrue(released[0].key.object == 1 && released[0].owner == 0);

            erased.clear();
            t.ExpireClaimable(3000, 3000, erased);
            Assert::AreEqual((size_t)0, erased.size(), L"Still inside the claim window.");
            t.ExpireClaimable(4500, 3000, erased);
            Assert::AreEqual((size_t)1, erased.size(), L"Nobody claimed it: erased.");
            Assert::AreEqual((size_t)0, t.Size());
        }
    };

    TEST_CLASS(HandshakeTests)
    {
    public:
        TEST_METHOD(GameVersionMismatchIsRefused)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            host.SetGameVersion(42);
            Assert::IsTrue(host.StartHost(port, 1));
            net::Session other;
            other.SetGameVersion(41);
            Assert::IsTrue(other.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &other }, [&] { return other.GetRefusal() != net::REFUSE_NONE; }));
            Assert::AreEqual((int)net::REFUSE_GAME_VERSION, (int)other.GetRefusal());
            Assert::IsFalse(other.IsInMatch(), L"A refused session has left the match.");

            net::Session same;
            same.SetGameVersion(42);
            Assert::IsTrue(same.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &same }, [&] { return same.IsWelcomed(); }));
            same.Leave(); host.Leave();
        }

        TEST_METHOD(ForeignProtocolAndSilentPeersAreRefused)
        {
            unsigned short port = PickFreePort();
            net::ServerCore core;
            core.SetHandshakeTimeoutMs(150);
            Assert::IsTrue(core.Start(port, 1));

            // A raw client with a wrong protocol version.
            net::ClientEndpoint bad;
            std::vector<net::PacketId> badGot;
            bad.SetPacketSink([&](const net::ClientEndpoint::RawPacket& rp) { badGot.push_back(rp.id); });
            Assert::IsTrue(bad.Connect("127.0.0.1", port));
            // A raw client that never says hello.
            net::ClientEndpoint silent;
            std::vector<net::PacketId> silentGot;
            silent.SetPacketSink([&](const net::ClientEndpoint::RawPacket& rp) { silentGot.push_back(rp.id); });
            Assert::IsTrue(silent.Connect("127.0.0.1", port));

            bool helloSent = false;
            for (int i = 0; i < 1500 && (badGot.empty() || silentGot.empty()); ++i)
            {
                core.Update(); bad.Update(); silent.Update();
                if (!helloSent && bad.IsPeerReady())
                {
                    uint8_t hello[16];
                    net::Writer w(hello, sizeof(hello));
                    w.WriteU32(net::PROTOCOL_MAGIC); w.WriteU16(net::PROTOCOL_VERSION + 7); w.WriteU32(0); w.WriteU8(0);
                    bad.SendTo(net::PID_HELLO, hello, w.Position());
                    helloSent = true;
                }
                Sleep(1);
            }
            Assert::IsTrue(!badGot.empty() && badGot[0] == net::PID_REFUSE, L"Wrong protocol: REFUSE, not WELCOME.");
            Assert::IsTrue(!silentGot.empty() && silentGot[0] == net::PID_REFUSE, L"No HELLO: refused after the timeout.");
            Assert::AreEqual((size_t)0, core.GetAdmittedPeers().size());
            Assert::IsTrue(core.GetStats().refused >= 2);
            bad.Disconnect(); silent.Disconnect(); core.Stop();
        }

        TEST_METHOD(DedicatedCoreHasNoLoopbackPlayer)
        {
            // The dedicated server is the bare ServerCore: it takes no slot and
            // a lone joiner sees nobody else.
            unsigned short port = PickFreePort();
            net::ServerCore core;
            core.SetMaxPeers(1);
            Assert::IsTrue(core.Start(port, 0x77));
            net::Session a;
            Assert::IsTrue(a.Join("127.0.0.1", port));
            for (int i = 0; i < 2000 && !a.IsWelcomed(); ++i) { core.Update(); a.Update(); Sleep(1); }
            Assert::IsTrue(a.IsWelcomed(), L"The only slot is the player's.");
            Assert::AreEqual((size_t)0, a.GetPeerRoster().size());
            Assert::AreEqual((uint32_t)0x77, a.GetWorldSeed());
            a.Leave(); core.Stop();
        }
    };

    TEST_CLASS(SessionTransportTests)
    {
    public:
        TEST_METHOD(ClockIsSyncedAndStreamsAreTimestamped)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 1));
            struct Provider : net::ILocalStateProvider
            {
                bool GetLocalState(uint8_t* out, uint16_t cap, uint16_t& size) override
                { if (cap < 1) return false; out[0] = 1; size = 1; return true; }
            } prov;
            struct Sink : net::IRemoteStateSink
            {
                uint32_t lastTime = 0; int got = 0;
                void ApplyRemoteState(net::PeerId, const uint8_t*, uint16_t, uint32_t t) override { lastTime = t; ++got; }
                void OnPeerLeft(net::PeerId) override {}
            } sink;
            net::Session a, b;
            a.SetLocalStateProvider(&prov);
            b.SetRemoteStateSink(&sink);
            Assert::IsTrue(a.Join("127.0.0.1", port));
            Assert::IsTrue(b.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &a, &b }, [&] { return sink.got > 2 && b.GetRttMs() > 0; }));
            uint32_t serverNow = host.GetServerCore().NowMs();
            Assert::IsTrue(b.IsClockSynced());
            int32_t clockErr = static_cast<int32_t>(b.ServerTimeMs() - serverNow);
            Assert::IsTrue(clockErr > -50 && clockErr < 50, L"Client estimate of the server clock.");
            int32_t age = static_cast<int32_t>(serverNow - sink.lastTime);
            Assert::IsTrue(age >= -50 && age < 500, L"Relayed state carries its server sample time.");
            b.Leave(); a.Leave(); host.Leave();
        }

        TEST_METHOD(JoinerSeesDeparturesAndLostConnection)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 1));
            struct Sink : net::IRemoteStateSink
            {
                std::vector<net::PeerId> left;
                void ApplyRemoteState(net::PeerId, const uint8_t*, uint16_t, uint32_t) override {}
                void OnPeerLeft(net::PeerId p) override { left.push_back(p); }
            } sink;
            std::vector<std::pair<net::PeerId, bool>> events;
            net::Session a, b;
            a.SetRemoteStateSink(&sink);
            a.SetJoinLeaveFn([&](net::PeerId p, bool j) { events.push_back({ p, j }); });
            Assert::IsTrue(a.Join("127.0.0.1", port));
            Assert::IsTrue(b.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &a, &b }, [&] { return a.GetPeerRoster().size() == 2; }),
                L"The joiner sees the host and the other joiner.");
            net::PeerId bId = b.GetLocalPeerId();
            b.Leave();
            Assert::IsTrue(PumpUntil({ &host, &a }, [&] { return !sink.left.empty(); }));
            Assert::AreEqual((int)bId, (int)sink.left[0], L"A joiner is told about a departure too.");
            bool sawJoin = false, sawLeave = false;
            for (auto& e : events) { if (e.first == bId) { sawJoin |= e.second; sawLeave |= !e.second; } }
            Assert::IsTrue(sawJoin && sawLeave);

            host.Leave(); // the server goes away
            Assert::IsTrue(PumpUntil({ &a }, [&] { return a.IsConnectionLost(); }, 40000));
            Assert::IsFalse(a.IsInMatch());
        }

        TEST_METHOD(FloodingPeerIsDroppedThenKicked)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            net::RateLimitConfig lim;
            lim.userPerSec = 10.0f; lim.userBurst = 5.0f;
            lim.violationsToKick = 50; lim.violationWindowMs = 10000;
            host.GetServerCore().SetRateLimits(lim);
            Assert::IsTrue(host.StartHost(port, 1));
            net::Session flooder, victim;
            int victimGot = 0;
            victim.SetCustomEventSink([&](net::PeerId, const uint8_t*, uint16_t) { ++victimGot; });
            Assert::IsTrue(flooder.Join("127.0.0.1", port));
            Assert::IsTrue(victim.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &flooder, &victim }, [&] { return flooder.IsWelcomed() && victim.IsWelcomed(); }));
            const uint8_t msg[4] = { 1, 2, 3, 4 };
            for (int i = 0; i < 200; ++i) flooder.SendCustomEvent(msg, sizeof(msg));
            PumpUntil({ &host, &flooder, &victim }, [&] { return flooder.GetRefusal() != net::REFUSE_NONE; });
            Assert::IsTrue(victimGot >= 5 && victimGot < 60, L"Only the budget gets through.");
            Assert::AreEqual((int)net::REFUSE_FLOOD, (int)flooder.GetRefusal(), L"A persistent flooder is kicked.");
            Assert::IsTrue(host.GetServerCore().GetStats().rateDropped > 0);
            victim.Leave(); flooder.Leave(); host.Leave();
        }

        TEST_METHOD(EntityEventsGoToTheOwnerOnly)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 1));
            net::Session owner, hitter, bystander;
            int ownerGot = 0, bystanderGot = 0;
            uint8_t kind = 0;
            owner.SetEntityEventSink([&](net::PeerId, uint64_t, uint8_t k, const uint8_t*, uint16_t) { ++ownerGot; kind = k; });
            bystander.SetEntityEventSink([&](net::PeerId, uint64_t, uint8_t, const uint8_t*, uint16_t) { ++bystanderGot; });
            for (net::Session* s : { &owner, &hitter, &bystander }) Assert::IsTrue(s->Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &owner, &hitter, &bystander },
                [&] { return owner.IsWelcomed() && hitter.IsWelcomed() && bystander.IsWelcomed(); }));
            uint64_t ent = net::MakeDynamicEntityId(owner.GetLocalPeerId(), 1);
            bool known = false;
            hitter.SetWorldStateSink([&](const net::WorldStateEntry& e, net::WorldStateReason, net::PeerId) { known |= e.key.object == ent; });
            owner.RequestWorldState(MakeOp(ent, net::WS_CHANNEL_ENTITY, net::WSP_OWNER, 1, net::WSO_SET, net::WSF_ERASE_ON_OWNER_LEAVE));
            Assert::IsTrue(PumpUntil({ &host, &owner, &hitter, &bystander }, [&] { return known; }));
            const uint8_t payload[3] = { 9, 9, 9 };
            Assert::IsTrue(hitter.SendEntityEvent(ent, net::NEE_HIT, payload, sizeof(payload)));
            Assert::IsTrue(PumpUntil({ &host, &owner, &hitter, &bystander }, [&] { return ownerGot > 0; }));
            PumpAll(host, { &owner, &hitter, &bystander }, 50);
            Assert::AreEqual(1, ownerGot);
            Assert::AreEqual((int)net::NEE_HIT, (int)kind);
            Assert::AreEqual(0, bystanderGot, L"Nobody but the owner gets the hit.");
            bystander.Leave(); hitter.Leave(); owner.Leave(); host.Leave();
        }
    };

    TEST_CLASS(GameServerTests)
    {
    public:
        TEST_METHOD(MovementValidatorRules)
        {
            PlayerMovementValidator v;
            std::vector<uint8_t> corr;
            auto check = [&](const PlayerNetworkState& st, uint32_t t) {
                std::vector<uint8_t> b = Blob(st);
                corr.clear();
                return v.ValidatePeerState(1, b.data(), (uint16_t)b.size(), t, corr);
            };
            Assert::AreEqual((int)net::SV_ACCEPT, (int)check(StateAt(0, 0), 1000), L"First state: spawn.");
            Assert::AreEqual((int)net::SV_ACCEPT, (int)check(StateAt(1.5f, 0), 1100), L"Running pace.");
            Assert::AreEqual((int)net::SV_CORRECT, (int)check(StateAt(500, 0), 1200), L"Teleport refused...");
            PlayerNetworkState back;
            Assert::IsTrue(back.Deserialize(reinterpret_cast<const char*>(corr.data()), static_cast<int>(corr.size())));
            Assert::AreEqual(1.5f, back.x, L"... with the last accepted state to snap back to.");
            Assert::AreEqual((int)net::SV_DROP, (int)check(StateAt(500, 0), 1300), L"No correction spam.");
            Assert::AreEqual((int)net::SV_ACCEPT, (int)check(StateAt(2.0f, 0), 1400), L"Back on track.");
            // Declared teleports pass, within their budget (burst 3).
            Assert::AreEqual((int)net::SV_ACCEPT, (int)check(StateAt(900, 0, 1), 1500));
            Assert::AreEqual((int)net::SV_ACCEPT, (int)check(StateAt(1900, 0, 2), 1600));
            Assert::AreEqual((int)net::SV_ACCEPT, (int)check(StateAt(2900, 0, 3), 1700));
            Assert::AreNotEqual((int)net::SV_ACCEPT, (int)check(StateAt(3900, 0, 4), 1800), L"Teleport rate limited.");
            // Garbage never passes: a truncated record is dropped, and a
            // non-finite position cannot even be encoded.
            corr.clear();
            std::vector<uint8_t> cut = Blob(StateAt(3000, 0, 3));
            Assert::AreEqual((int)net::SV_DROP, (int)v.ValidatePeerState(1, cut.data(), (uint16_t)(cut.size() - 1), 5000, corr));
            PlayerNetworkState nan;
            std::vector<uint8_t> nb = Blob(StateAt(std::numeric_limits<float>::quiet_NaN(), 0));
            Assert::IsTrue(nan.Deserialize(reinterpret_cast<const char*>(nb.data()), (int)nb.size()) && nan.x == 0.0f);
        }

        TEST_METHOD(ValidatorCorrectsTheClientThroughTheSession)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            GameServer rules;
            GameServer::Options opt;
            rules.Attach(host.GetServerCore(), opt);
            Assert::IsTrue(host.StartHost(port, 1));
            struct Provider : net::ILocalStateProvider
            {
                PlayerNetworkState st = StateAt(0, 0);
                bool GetLocalState(uint8_t* out, uint16_t cap, uint16_t& size) override
                {
                    if (cap < PlayerNetworkState::MAX_SERIALIZED_SIZE) return false;
                    size = static_cast<uint16_t>(st.Serialize(reinterpret_cast<char*>(out)));
                    return size > 0;
                }
            } prov;
            net::Session cheater;
            cheater.SetLocalStateProvider(&prov);
            cheater.SetStateSendIntervalMs(20);
            int corrections = 0;
            cheater.SetStateCorrectionSink([&](const uint8_t*, uint16_t) { ++corrections; });
            Assert::IsTrue(cheater.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &cheater }, [&] { return rules.Movement().Accepted() > 0; }));
            prov.st.x = 5000.0f;
            Assert::IsTrue(PumpUntil({ &host, &cheater }, [&] { return corrections > 0; }));
            Assert::IsTrue(host.GetServerCore().GetStats().corrections > 0);
            cheater.Leave(); host.Leave();
        }

        TEST_METHOD(PvpOptionGatesPlayerEvents)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            GameServer rules;
            GameServer::Options opt;
            Assert::IsTrue(opt.pvp, L"PvP is on unless the server turns it off.");
            opt.pvp = false;
            rules.Attach(host.GetServerCore(), opt);
            Assert::IsTrue(host.StartHost(port, 1));
            net::Session a, b;
            int bHits = 0;
            uint32_t bOptions = 0xFFFFFFFFu;
            b.SetPlayerEventSink([&](net::PeerId, uint8_t, const uint8_t*, uint16_t) { ++bHits; });
            b.SetSessionOptionsFn([&](uint32_t o) { bOptions = o; });
            Assert::IsTrue(a.Join("127.0.0.1", port));
            Assert::IsTrue(b.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &a, &b }, [&] { return a.IsWelcomed() && b.IsWelcomed(); }));
            Assert::AreEqual(0u, bOptions & net::SO_PVP);

            const uint8_t hit[2] = { 1, 2 };
            a.SendPlayerEvent(b.GetLocalPeerId(), 2, hit, sizeof(hit));
            PumpAll(host, { &a, &b }, 100);
            Assert::AreEqual(0, bHits, L"No PvP: hits are not relayed.");

            rules.SetPvp(true);
            Assert::IsTrue(PumpUntil({ &host, &a, &b }, [&] { return (b.GetSessionOptions() & net::SO_PVP) != 0; }),
                L"The option change reaches every client.");
            a.SendPlayerEvent(b.GetLocalPeerId(), 2, hit, sizeof(hit));
            Assert::IsTrue(PumpUntil({ &host, &a, &b }, [&] { return bHits == 1; }), L"PvP: the victim gets the hit.");
            b.Leave(); a.Leave(); host.Leave();
        }
    };

    namespace
    {
        struct MigGhost { uint64_t id; net::NetEntityMotion last; };
        std::vector<MigGhost*> g_migGhosts;
        int g_adopted = 0;
        int g_hits = 0;
        void* MigSpawn(uint64_t id, const net::NetEntitySpawn&) { MigGhost* g = new MigGhost{ id, {} }; g_migGhosts.push_back(g); return g; }
        void MigMove(void* h, const net::NetEntityMotion& m) { static_cast<MigGhost*>(h)->last = m; }
        void MigDespawn(void* h, uint8_t)
        {
            for (size_t i = 0; i < g_migGhosts.size(); ++i)
                if (g_migGhosts[i] == h) { delete g_migGhosts[i]; g_migGhosts.erase(g_migGhosts.begin() + i); return; }
        }
        int g_adoptedCar = 0;
        void* MigAdopt(uint64_t, void* ghost, const net::NetEntitySpawn&, const net::NetEntityMotion&)
        {
            MigDespawn(ghost, 0);
            ++g_adopted;
            return &g_adoptedCar;
        }
        void MigHit(void*, const net::NetEntityHit& h) { if (h.damage == 99) ++g_hits; }
        bool MigSample(void*, net::NetEntityMotion& m) { m.x = 50.0f; m.z = 0.0f; return true; }
    }

    TEST_CLASS(EntityMigrationTests)
    {
    public:
        TEST_METHOD(ReleasedWhenSomeoneIsNearAndClaimedByTheNearPlayer)
        {
            NetEntityManager owner, taker;
            std::vector<net::WorldStateOp> ownerOps, takerOps;
            owner.SetSenders([&](const net::WorldStateOp& op) { ownerOps.push_back(op); return true; },
                             NetEntityManager::SendStatesFn());
            taker.SetSenders([&](const net::WorldStateOp& op) { takerOps.push_back(op); return true; },
                             NetEntityManager::SendStatesFn());
            owner.SetSampler(&MigSample);
            NetEntityManager::Sink s = { &MigSpawn, &MigMove, &MigDespawn, &MigAdopt, &MigHit };
            taker.SetSink(s);
            owner.SetPresence(NetEntityManager::LocalPlayerFn(),
                              [](float x, float, float r) { return x <= r + 60.0f; }, // the taker stands at x=60
                              NetEntityManager::CanAdoptFn());
            taker.SetPresence([](float& x, float& z) { x = 60.0f; z = 0.0f; return true; },
                              NetEntityManager::RemotePlayerNearFn(),
                              [](uint8_t, float, float, float) { return true; });
            owner.OnWelcomed(1);
            taker.OnWelcomed(2);

            int car = 0;
            net::NetEntitySpawn sp; sp.kind = net::NEK_TRAFFIC_CAR; strcpy(sp.model, "compactA");
            owner.AddLocal(&car, sp);
            Assert::AreEqual((size_t)1, ownerOps.size());
            uint64_t id = ownerOps[0].key.object;

            owner.RemoveLocal(&car); // leaves the owner's range, the taker is near it
            Assert::AreEqual((int)net::WSO_RELEASE, (int)ownerOps.back().kind, L"Handed over, not erased.");
            net::NetEntitySpawn pose;
            Assert::IsTrue(pose.Decode(ownerOps.back().value) && pose.x == 50.0f, L"Released at its current pose.");

            // The server's broadcast of the release: claimable, owner 0.
            net::WorldStateEntry e;
            e.key = ownerOps[0].key; e.owner = 0; e.policy = net::WSP_OWNER; e.flags = net::WSF_CLAIMABLE;
            e.value = ownerOps.back().value;
            taker.OnAuthoritative(e, net::WSR_REMOTE);
            Assert::AreEqual((size_t)1, taker.RemoteCount(), L"Shown as a ghost meanwhile.");
            taker.Update(0.016f);
            Assert::AreEqual((size_t)1, takerOps.size());
            Assert::AreEqual((int)net::WSO_CLAIM, (int)takerOps[0].kind, L"The near player claims it.");
            taker.Update(0.016f);
            Assert::AreEqual((size_t)1, takerOps.size(), L"Claimed once.");

            // Granted: the ghost becomes our simulated entity with the same id.
            e.owner = 2; e.flags = 0;
            taker.OnAuthoritative(e, net::WSR_CONFIRM);
            Assert::AreEqual(1, g_adopted);
            Assert::AreEqual((size_t)0, taker.RemoteCount());
            Assert::AreEqual(id, taker.LocalId(&g_adoptedCar));

            // Hits on it now reach us.
            net::NetEntityHit hit; hit.damage = 99;
            uint8_t buf[net::NetEntityHit::SIZE];
            uint16_t n = hit.Encode(buf, sizeof(buf));
            taker.OnEntityEvent(1, id, net::NEE_HIT, buf, n);
            Assert::AreEqual(1, g_hits);
        }

        TEST_METHOD(ClaimOnlyWhatWeWouldKeep)
        {
            // A released car is claimed only if our own game would keep it at
            // its pose (otherwise two players hand it back and forth forever).
            for (MigGhost* g : g_migGhosts) delete g;
            g_migGhosts.clear();
            NetEntityManager taker;
            std::vector<net::WorldStateOp> ops;
            taker.SetSenders([&](const net::WorldStateOp& op) { ops.push_back(op); return true; },
                             NetEntityManager::SendStatesFn());
            NetEntityManager::Sink s = { &MigSpawn, &MigMove, &MigDespawn, &MigAdopt, &MigHit };
            taker.SetSink(s);
            float askedX = -1.0f;
            bool keep = false;
            taker.SetPresence([](float& x, float& z) { x = 0.0f; z = 0.0f; return true; },
                              NetEntityManager::RemotePlayerNearFn(),
                              [&](uint8_t, float x, float, float) { askedX = x; return keep; });
            taker.OnWelcomed(2);

            net::NetEntitySpawn sp; sp.kind = net::NEK_TRAFFIC_CAR; strcpy(sp.model, "compactA"); sp.x = 40.0f;
            net::WorldStateEntry e;
            e.key.object = net::MakeDynamicEntityId(5, 1); e.key.channel = net::WS_CHANNEL_ENTITY;
            e.owner = 0; e.policy = net::WSP_OWNER; e.flags = net::WSF_CLAIMABLE;
            sp.Encode(e.value);
            taker.OnAuthoritative(e, net::WSR_REMOTE);
            taker.Update(0.016f);
            Assert::AreEqual(40.0f, askedX, 0.01f, L"Asked with the car's pose.");
            Assert::AreEqual((size_t)0, ops.size(), L"Not claimed: we would remove it again.");
            keep = true;
            taker.Update(0.016f);
            Assert::AreEqual((size_t)1, ops.size());
            Assert::AreEqual((int)net::WSO_CLAIM, (int)ops[0].kind);
            taker.OnMatchEnd();
        }

        TEST_METHOD(NobodyNearMeansErase)
        {
            NetEntityManager owner;
            std::vector<net::WorldStateOp> ops;
            owner.SetSenders([&](const net::WorldStateOp& op) { ops.push_back(op); return true; },
                             NetEntityManager::SendStatesFn());
            owner.SetSampler(&MigSample);
            owner.SetPresence(NetEntityManager::LocalPlayerFn(),
                              [](float, float, float) { return false; }, NetEntityManager::CanAdoptFn());
            owner.OnWelcomed(1);
            int car = 0;
            net::NetEntitySpawn sp; sp.kind = net::NEK_TRAFFIC_CAR;
            owner.AddLocal(&car, sp);
            owner.RemoveLocal(&car);
            Assert::AreEqual((int)net::WSO_ERASE, (int)ops.back().kind);
        }

        TEST_METHOD(GhostsRenderFromTimedSamples)
        {
            static uint32_t clock = 0;
            NetEntityManager m;
            NetEntityManager::Sink s = { &MigSpawn, &MigMove, &MigDespawn, nullptr, nullptr };
            m.SetSink(s);
            m.SetClock([]() { return clock; });
            m.SetInterpolationDelayMs(100);
            m.OnWelcomed(1);
            net::NetEntitySpawn sp; sp.kind = net::NEK_TRAFFIC_CAR;
            net::WorldStateEntry e; e.key.object = net::MakeDynamicEntityId(2, 1); e.key.channel = net::WS_CHANNEL_ENTITY;
            e.owner = 2; e.policy = net::WSP_OWNER; sp.Encode(e.value);
            m.OnAuthoritative(e, net::WSR_REMOTE);
            net::NetEntityMotion a; a.x = 0.0f;
            net::NetEntityMotion b; b.x = 10.0f;
            net::EntityStateRecord ra, rb;
            a.Encode(ra, e.key.object); b.Encode(rb, e.key.object);
            m.OnEntityState(2, ra, 1000);
            m.OnEntityState(2, rb, 1100);
            clock = 1150;
            m.Update(0.016f);
            MigGhost* g = g_migGhosts.back();
            Assert::AreEqual(5.0f, g->last.x, 0.01f);
            m.OnMatchEnd();
        }
    };

    namespace
    {
        int g_pvSpawns = 0, g_pvDespawns = 0, g_pvRestyles = 0;
        void* PvSpawn(int id) { ++g_pvSpawns; return reinterpret_cast<void*>(static_cast<size_t>(0x3000 + id)); }
        void PvMove(int, void*, const PlayerNetworkState&, const RemoteMotion&) {}
        void PvDespawn(int, void*) { ++g_pvDespawns; }
        void PvRestyle(int, void*) { ++g_pvRestyles; }
    }

    TEST_CLASS(PlayerPresenceTests)
    {
    public:
        TEST_METHOD(PublishesOnWelcomeAndOnChangeOnly)
        {
            PlayerPresence p;
            std::vector<net::WorldStateOp> ops;
            p.SetSender([&](const net::WorldStateOp& op) { ops.push_back(op); return true; });
            net::PlayerContext c; c.level = 2; c.zone = 0;
            p.SetLocalContext(c);
            net::PlayerAppearance a; a.Set("homer", "homer");
            p.SetLocalAppearance(a);
            Assert::AreEqual((size_t)0, ops.size(), L"Nothing before our peer id is known.");
            p.OnWelcomed(4);
            Assert::AreEqual((size_t)2, ops.size());
            Assert::IsTrue(ops[0].key.object == net::MakePlayerObjectId(4) && ops[0].policy == net::WSP_OWNER
                           && (ops[0].flags & net::WSF_ERASE_ON_OWNER_LEAVE));
            p.SetLocalContext(c);
            p.SetLocalAppearance(a);
            Assert::AreEqual((size_t)2, ops.size(), L"Unchanged facts are not resent.");
            c.zone = 0xABCDEF0123ull; // entered an interior
            p.SetLocalContext(c);
            a.Set("h_evil", "homer"); // a costume
            p.SetLocalAppearance(a);
            Assert::AreEqual((size_t)4, ops.size());
            Assert::AreEqual((int)net::WOC_APPEARANCE, (int)ops[3].key.channel);
            net::PlayerAppearance back;
            Assert::IsTrue(back.Decode(ops[3].value) && std::string(back.model) == "h_evil");
            net::PlayerContext cb;
            Assert::IsTrue(cb.Decode(ops[2].value) && cb.zone == 0xABCDEF0123ull && cb.level == 2);
        }

        TEST_METHOD(RemoteFactsDriveVisibilityAndRestyle)
        {
            PlayerPresence p;
            p.SetSender([](const net::WorldStateOp&) { return true; });
            net::PlayerContext mine; mine.level = 1; mine.zone = 0;
            p.SetLocalContext(mine);
            p.OnWelcomed(1);

            auto record = [](net::PeerId peer, uint8_t ch, const net::WorldStateValue& v) {
                net::WorldStateEntry e;
                e.key.object = net::MakePlayerObjectId(peer); e.key.channel = ch;
                e.owner = peer; e.policy = net::WSP_OWNER; e.value = v;
                return e;
            };
            Assert::IsTrue(p.SharesContext(2), L"Unknown context: shown.");
            net::PlayerContext inside; inside.level = 1; inside.zone = 77;
            net::WorldStateValue v; inside.Encode(v);
            p.OnAuthoritative(record(2, net::WOC_PLAYER_CONTEXT, v), net::WSR_REMOTE);
            Assert::IsFalse(p.SharesContext(2), L"In an interior while we are outside.");

            RemotePlayerWorldView view;
            RemotePlayerWorldView::Sink sink;
            std::memset(&sink, 0, sizeof(sink));
            sink.spawn = &PvSpawn; sink.move = &PvMove; sink.despawn = &PvDespawn; sink.restyle = &PvRestyle;
            view.SetSink(sink);
            view.SetPresence([&](int id) { return p.SharesContext((net::PeerId)id); },
                             [&](int id) { return p.AppearanceVersion((net::PeerId)id); });
            g_pvSpawns = g_pvDespawns = g_pvRestyles = 0;
            RemotePlayerManager players;
            players.UpdatePlayer(2, StateAt(0, 0));
            view.Sync(players);
            Assert::AreEqual(0, g_pvSpawns, L"No puppet outside our context.");

            net::PlayerContext out; out.level = 1; out.zone = 0;
            out.Encode(v);
            p.OnAuthoritative(record(2, net::WOC_PLAYER_CONTEXT, v), net::WSR_REMOTE);
            view.Sync(players);
            Assert::AreEqual(1, g_pvSpawns, L"Back outside: shown.");

            net::PlayerAppearance a; a.Set("marge", "marge");
            net::WorldStateValue av; a.Encode(av);
            p.OnAuthoritative(record(2, net::WOC_APPEARANCE, av), net::WSR_REMOTE);
            Assert::IsTrue(p.Appearance(2) != nullptr && std::string(p.Appearance(2)->model) == "marge");
            view.Sync(players);
            Assert::AreEqual(1, g_pvRestyles, L"A new appearance restyles in place.");
            Assert::AreEqual(1, g_pvSpawns);
            view.Sync(players);
            Assert::AreEqual(1, g_pvRestyles, L"Once per change.");

            inside.Encode(v);
            p.OnAuthoritative(record(2, net::WOC_PLAYER_CONTEXT, v), net::WSR_REMOTE);
            view.Sync(players);
            Assert::AreEqual(1, g_pvDespawns, L"Entering an interior hides the puppet.");
            view.Clear();
            Assert::AreEqual(1, g_pvDespawns, L"A hidden player has nothing to despawn.");
        }

        TEST_METHOD(GarbageAppearanceIsRejected)
        {
            net::PlayerAppearance a;
            net::WorldStateValue v; v.len = 5;
            Assert::IsFalse(a.Decode(v));
            net::WorldStateValue big; big.len = 48;
            std::memset(big.bytes, 'x', 48);
            Assert::IsTrue(a.Decode(big));
            Assert::AreEqual((size_t)31, std::strlen(a.model), L"Terminator enforced.");
        }
    };

    namespace
    {
        net::WorldStateOp ContextOp(net::PeerId peer, uint8_t level, uint64_t zone)
        {
            net::WorldStateOp op;
            op.key.object = net::MakePlayerObjectId(peer);
            op.key.channel = net::WOC_PLAYER_CONTEXT;
            op.kind = net::WSO_SET;
            op.policy = net::WSP_OWNER;
            op.flags = net::WSF_ERASE_ON_OWNER_LEAVE;
            net::PlayerContext c;
            c.level = level;
            c.zone = zone;
            c.Encode(op.value);
            return op;
        }

        void Place(PlayerInterestPolicy& p, net::PeerId peer, float x, float z)
        {
            std::vector<uint8_t> b = Blob(StateAt(x, z));
            p.OnPlayerState(peer, b.data(), static_cast<uint16_t>(b.size()), 0);
        }

        uint32_t g_entityClock = 0;
    }

    TEST_CLASS(InterestTests)
    {
    public:
        TEST_METHOD(PolicyTiersPlayersByContextAndDistance)
        {
            net::WorldStateTable world;
            PlayerInterestPolicy policy;
            policy.SetWorld(&world);
            net::WorldStateEntry e;
            bool existed = false;
            world.Apply(1, ContextOp(1, 1, 0), e, existed);
            world.Apply(2, ContextOp(2, 1, 0), e, existed);
            world.Apply(3, ContextOp(3, 1, 0x77), e, existed); // inside an interior
            // peer 4 has no context record yet

            Place(policy, 1, 0.0f, 0.0f);
            Place(policy, 2, 100.0f, 0.0f);
            Place(policy, 3, 5.0f, 0.0f);
            Place(policy, 4, 5000.0f, 0.0f);

            Assert::AreEqual(0u, policy.PlayerStateIntervalMs(1, 2), L"Near: every sample.");
            Assert::AreEqual(PlayerInterestPolicy::kNotRelevant, policy.PlayerStateIntervalMs(1, 3),
                             L"Another context: nothing, however close.");
            Assert::AreEqual(PlayerInterestPolicy::kNotRelevant, policy.PlayerStateIntervalMs(3, 1));
            Assert::AreEqual(200u, policy.PlayerStateIntervalMs(1, 4), L"Unknown context is relevant; far = 5 Hz.");

            // Hysteresis: a near pair stays near up to nearRadius + 25 m.
            Place(policy, 2, 170.0f, 0.0f);
            Assert::AreEqual(0u, policy.PlayerStateIntervalMs(1, 2));
            Place(policy, 2, 180.0f, 0.0f);
            Assert::AreEqual(200u, policy.PlayerStateIntervalMs(1, 2));
            Place(policy, 2, 160.0f, 0.0f);
            Assert::AreEqual(200u, policy.PlayerStateIntervalMs(1, 2), L"Far again only below nearRadius.");
            Place(policy, 2, 140.0f, 0.0f);
            Assert::AreEqual(0u, policy.PlayerStateIntervalMs(1, 2));

            // Entities: in the owner's context and within entityRadius of the receiver.
            net::NetEntityMotion m;
            net::EntityStateRecord rec;
            m.x = 290.0f; m.Encode(rec, 42);
            Assert::IsTrue(policy.EntityRelevant(1, 2, rec));
            Assert::IsFalse(policy.EntityRelevant(3, 2, rec), L"Not inside the interior.");
            m.x = 320.0f; m.Encode(rec, 42);
            Assert::IsFalse(policy.EntityRelevant(1, 2, rec), L"Beyond entityRadius.");

            // A receiver without a known position gets everything; a peer that
            // left is forgotten.
            policy.OnPeerLeft(1);
            Assert::AreEqual(0u, policy.PlayerStateIntervalMs(1, 4));
            Assert::IsTrue(policy.EntityRelevant(1, 2, rec));

            PlayerInterestPolicy::Config off;
            off.enabled = false;
            policy.SetConfig(off);
            Assert::AreEqual(0u, policy.PlayerStateIntervalMs(3, 2), L"Disabled: everything to everyone.");
        }

        TEST_METHOD(ServerForwardsByInterestAndCountsTraffic)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            GameServer rules;
            GameServer::Options opt;
            rules.Attach(host.GetServerCore(), opt);
            Assert::IsTrue(host.StartHost(port, 1));

            struct Provider : net::ILocalStateProvider
            {
                PlayerNetworkState st;
                bool GetLocalState(uint8_t* out, uint16_t cap, uint16_t& size) override
                {
                    if (cap < PlayerNetworkState::MAX_SERIALIZED_SIZE) return false;
                    size = static_cast<uint16_t>(st.Serialize(reinterpret_cast<char*>(out)));
                    return size > 0;
                }
            };
            struct Sink : net::IRemoteStateSink
            {
                std::map<net::PeerId, int> got;
                void ApplyRemoteState(net::PeerId from, const uint8_t*, uint16_t, uint32_t) override { ++got[from]; }
                void OnPeerLeft(net::PeerId) override {}
            } sink;

            // a at the origin; b near it; c 1 km away; d next to a but indoors.
            Provider pa, pb, pc, pd;
            pa.st = StateAt(0.0f, 0.0f);
            pb.st = StateAt(20.0f, 0.0f);
            pc.st = StateAt(1000.0f, 0.0f);
            pd.st = StateAt(10.0f, 0.0f);
            net::Session a, b, c, d;
            a.SetRemoteStateSink(&sink);
            net::Session* joiners[4] = { &a, &b, &c, &d };
            Provider* provs[4] = { &pa, &pb, &pc, &pd };
            for (int i = 0; i < 4; ++i)
            {
                joiners[i]->SetLocalStateProvider(provs[i]);
                joiners[i]->SetStateSendIntervalMs(50);
                Assert::IsTrue(joiners[i]->Join("127.0.0.1", port));
            }
            Assert::IsTrue(PumpUntil({ &host, &a, &b, &c, &d }, [&] {
                return a.IsWelcomed() && b.IsWelcomed() && c.IsWelcomed() && d.IsWelcomed();
            }));
            for (int i = 0; i < 4; ++i)
                joiners[i]->RequestWorldState(ContextOp(joiners[i]->GetLocalPeerId(), 1, i == 3 ? 0x99u : 0u));
            const net::WorldStateTable& world = host.GetServerCore().GetWorldStateTable();
            Assert::IsTrue(PumpUntil({ &host, &a, &b, &c, &d }, [&] {
                for (int i = 0; i < 4; ++i)
                {
                    net::WorldStateKey k;
                    k.object = net::MakePlayerObjectId(joiners[i]->GetLocalPeerId());
                    k.channel = net::WOC_PLAYER_CONTEXT;
                    net::WorldStateEntry e;
                    if (!world.Find(k, e)) return false;
                }
                return sink.got.count(b.GetLocalPeerId()) && sink.got.count(c.GetLocalPeerId());
            }), L"Contexts recorded, streams flowing.");

            PumpAll(host, { &a, &b, &c, &d }, 50);
            sink.got.clear();
            uint32_t t0 = host.GetServerCore().NowMs();
            net::TrafficStats before = *host.GetServerCore().GetTraffic();
            PumpUntil({ &host, &a, &b, &c, &d }, [&] { return host.GetServerCore().NowMs() - t0 >= 2000; }, 20000);
            double secs = (host.GetServerCore().NowMs() - t0) / 1000.0;
            double nearRate = sink.got[b.GetLocalPeerId()] / secs;
            double farRate = sink.got[c.GetLocalPeerId()] / secs;
            int indoor = sink.got[d.GetLocalPeerId()];

            Assert::AreEqual(0, indoor, L"A player in another context is never sent.");
            Assert::IsTrue(nearRate > 12.0, L"A near player arrives at the full 20 Hz.");
            Assert::IsTrue(farRate > 2.5 && farRate < 7.0, L"A far player arrives at about 5 Hz.");
            Assert::IsTrue(host.GetServerCore().GetStats().samplesFiltered > 0);

            // Traffic is counted per packet id: every player sample is the
            // 4-byte frame + [u16 from][u32 time] + the 21-byte on-foot state.
            const net::TrafficStats& after = *host.GetServerCore().GetTraffic();
            const net::TrafficCounter& out = after.sent.at(net::PID_PLAYER_STATE);
            const net::TrafficCounter& was = before.sent.at(net::PID_PLAYER_STATE);
            uint64_t pk = out.packets - was.packets;
            Assert::IsTrue(pk > 0);
            Assert::AreEqual<uint64_t>(pk * (4 + 6 + 21), out.bytes - was.bytes);
            Assert::IsTrue(after.wireSent.bytes > before.wireSent.bytes, L"ENet wire totals are collected.");
            for (net::Session* s : joiners) s->Leave();
            host.Leave();
            rules.Detach();
        }

        TEST_METHOD(AdaptiveDelayFollowsTheSampleSpacingAndGapsReset)
        {
            static uint32_t clock = 0;
            RemoteStateTrack::Timing timing;
            timing.clock = []() -> uint32_t { return clock; };
            timing.interpolationDelayMs = 100;
            timing.adaptive = true;

            RemoteStateTrack track;
            PlayerNetworkState s = StateAt(0, 0);
            PlayerNetworkState out;
            uint32_t t = 1000;
            for (int i = 0; i < 40; ++i) // 20 Hz: the delay stays at its base
            {
                s.x = i * 0.25f;
                track.Push(t, s);
                clock = t + 10;
                track.Render(timing, out);
                t += 50;
            }
            Assert::AreEqual(100.0f, track.CurrentDelayMs(), 0.5f);

            for (int i = 0; i < 60; ++i) // 5 Hz (far tier): two intervals, reached gradually
            {
                track.Push(t, s);
                for (int f = 0; f < 12; ++f)
                {
                    clock = t + f * 16;
                    track.Render(timing, out);
                    if (i == 0 && f == 1) Assert::IsTrue(track.CurrentDelayMs() < 120.0f, L"No jump in the delay.");
                }
                t += 200;
            }
            Assert::AreEqual(400.0f, track.CurrentDelayMs(), 20.0f);

            // A sample 5 s after the last one starts a new history: the shown
            // pose is the new one, not a slide from the old position.
            PlayerNetworkState away = StateAt(500.0f, 0.0f);
            t += 5000;
            track.Push(t, away);
            clock = t + 1;
            Assert::IsTrue(track.Render(timing, out));
            Assert::AreEqual(500.0f, out.x, 0.01f);
        }

        TEST_METHOD(VehicleModelRidesPresence)
        {
            PlayerPresence p;
            std::vector<net::WorldStateOp> ops;
            p.SetSender([&](const net::WorldStateOp& op) { ops.push_back(op); return true; });
            net::PlayerVehicle v;
            v.Set(12, "famil_v");
            p.SetLocalVehicle(v);
            Assert::AreEqual((size_t)0, ops.size());
            p.OnWelcomed(3);
            Assert::AreEqual((size_t)1, ops.size(), L"Published on welcome.");
            p.SetLocalVehicle(v);
            Assert::AreEqual((size_t)1, ops.size(), L"Unchanged: not resent.");
            net::PlayerVehicle none;
            p.SetLocalVehicle(none); // got out
            Assert::AreEqual((size_t)2, ops.size());
            Assert::AreEqual((int)net::WOC_VEHICLE, (int)ops[1].key.channel);

            PlayerPresence other;
            other.OnWelcomed(9);
            net::WorldStateEntry e;
            e.key = ops[0].key;
            e.owner = 3;
            e.value = ops[0].value;
            other.OnAuthoritative(e, net::WSR_REMOTE);
            const net::PlayerVehicle* got = other.Vehicle(3);
            Assert::IsNotNull(got);
            Assert::AreEqual(12, got->netVehicleId);
            Assert::AreEqual(std::string("famil_v"), std::string(got->model));
            other.OnAuthoritative(e, net::WSR_ERASED);
            Assert::IsNull(other.Vehicle(3));
        }

        TEST_METHOD(EntityMotionIsCompactAndExact)
        {
            net::NetEntityMotion m;
            m.x = -1234.56f; m.y = 7.5f; m.z = 0.01f; m.yaw = 3.0f;
            m.speedKmh = 77; m.steer = -40; m.flags = net::NetEntityMotion::MF_BRAKE; m.damage = 9; m.state = 4;
            net::EntityStateRecord rec;
            m.Encode(rec, 5);
            Assert::AreEqual(17, (int)rec.len);
            Assert::AreEqual(17, (int)net::NetEntityMotion::SIZE);
            net::NetEntityMotion back;
            Assert::IsTrue(back.Decode(rec));
            Assert::AreEqual(m.x, back.x, 0.005f);
            Assert::AreEqual(m.y, back.y, 0.005f);
            Assert::AreEqual(m.z, back.z, 0.005f);
            Assert::AreEqual(m.yaw, back.yaw, 0.0002f);
            Assert::AreEqual(77, (int)back.speedKmh);
            Assert::AreEqual(-40, (int)back.steer);
            Assert::AreEqual(9, (int)back.damage);
            Assert::AreEqual(4, (int)back.state);
        }

        TEST_METHOD(EntityWithoutFreshSamplesIsNotShown)
        {
            // The server stops forwarding an entity once it is far from us; its
            // last known pose must not become a ghost when we walk there.
            for (MigGhost* g : g_migGhosts) delete g;
            g_migGhosts.clear();
            NetEntityManager m;
            NetEntityManager::Sink s = { &MigSpawn, &MigMove, &MigDespawn, &MigAdopt, &MigHit };
            m.SetSink(s);
            m.SetClock([]() -> uint32_t { return g_entityClock; });
            m.SetRelevanceRadius(200.0f);
            m.SetPresence([](float& x, float& z) { x = 0.0f; z = 0.0f; return true; },
                          NetEntityManager::RemotePlayerNearFn(), NetEntityManager::CanAdoptFn());
            m.OnWelcomed(2);

            g_entityClock = 10000;
            net::NetEntitySpawn sp; sp.kind = net::NEK_TRAFFIC_CAR; sp.x = 10.0f;
            net::WorldStateEntry e;
            e.key.object = net::MakeDynamicEntityId(5, 1);
            e.key.channel = net::WS_CHANNEL_ENTITY;
            e.owner = 5; e.policy = net::WSP_OWNER;
            sp.Encode(e.value);
            m.OnAuthoritative(e, net::WSR_REMOTE);
            m.Update(0.016f);
            Assert::AreEqual((size_t)0, g_migGhosts.size(), L"No sample yet: not shown.");

            net::NetEntityMotion mo; mo.x = 12.0f;
            net::EntityStateRecord rec;
            mo.Encode(rec, e.key.object);
            m.OnEntityState(5, rec, g_entityClock);
            g_entityClock += 200;
            m.Update(0.016f);
            Assert::AreEqual((size_t)1, g_migGhosts.size(), L"Fresh samples: shown.");

            g_entityClock += 2000; // nothing more arrives
            m.Update(0.016f);
            Assert::AreEqual((size_t)0, g_migGhosts.size(), L"Stale: hidden.");

            // Released: the record's final pose is authoritative again.
            e.owner = 0; e.flags = net::WSF_CLAIMABLE;
            sp.x = 30.0f; sp.Encode(e.value);
            m.OnAuthoritative(e, net::WSR_REMOTE);
            m.Update(0.016f);
            Assert::AreEqual((size_t)1, g_migGhosts.size());
            Assert::AreEqual(30.0f, g_migGhosts[0]->last.x, 0.01f);
            m.OnMatchEnd();
        }
    };

    TEST_CLASS(ObjectClaimTests)
    {
    public:
        TEST_METHOD(ClaimOutcomeNamesTheWinner)
        {
            net::WorldStateValue v;
            v.len = net::EncodeClaim(300, v.bytes); // > 255: both bytes matter
            Assert::AreEqual((int)net::CLAIM_OURS, (int)net::ClaimOutcomeOf(v, 300));
            Assert::AreEqual((int)net::CLAIM_THEIRS, (int)net::ClaimOutcomeOf(v, 44));
            Assert::AreEqual((int)net::CLAIM_THEIRS, (int)net::ClaimOutcomeOf(v, 0), L"Not in a match: never ours.");
            v.len = 1; v.bytes[0] = 1;
            Assert::AreEqual((int)net::CLAIM_NONE, (int)net::ClaimOutcomeOf(v, 1), L"Not a claim record.");
            v.len = net::EncodeClaim(0, v.bytes);
            Assert::AreEqual((int)net::CLAIM_NONE, (int)net::ClaimOutcomeOf(v, 0));
        }

        TEST_METHOD(TwoPlayersClaimTheSameCarAndBothLearnTheWinner)
        {
            unsigned short port = PickFreePort();
            net::Session host;
            Assert::IsTrue(host.StartHost(port, 1));
            net::Session a, b;
            const uint64_t car = 0x0000123400000005ull; // a static world object id
            net::WorldStateValue seenA, seenB;
            bool gotA = false, gotB = false;
            a.SetWorldStateSink([&](const net::WorldStateEntry& e, net::WorldStateReason, net::PeerId) {
                if (e.key.object == car && e.key.channel == 9) { seenA = e.value; gotA = true; }
            });
            b.SetWorldStateSink([&](const net::WorldStateEntry& e, net::WorldStateReason, net::PeerId) {
                if (e.key.object == car && e.key.channel == 9) { seenB = e.value; gotB = true; }
            });
            Assert::IsTrue(a.Join("127.0.0.1", port));
            Assert::IsTrue(b.Join("127.0.0.1", port));
            Assert::IsTrue(PumpUntil({ &host, &a, &b }, [&] { return a.IsWelcomed() && b.IsWelcomed(); }));

            // Both got in at once: each predicts its own claim.
            net::WorldStateOp op;
            op.key.object = car; op.key.channel = 9;
            op.kind = net::WSO_SET; op.policy = net::WSP_FIRST_WRITE;
            op.value.len = net::EncodeClaim(a.GetLocalPeerId(), op.value.bytes);
            Assert::IsTrue(a.RequestWorldState(op));
            op.value.len = net::EncodeClaim(b.GetLocalPeerId(), op.value.bytes);
            Assert::IsTrue(b.RequestWorldState(op));

            Assert::IsTrue(PumpUntil({ &host, &a, &b }, [&] {
                return gotA && gotB && seenA == seenB;
            }), L"Both converge on one claimant.");
            net::ClaimOutcome oa = net::ClaimOutcomeOf(seenA, a.GetLocalPeerId());
            net::ClaimOutcome ob = net::ClaimOutcomeOf(seenB, b.GetLocalPeerId());
            Assert::IsTrue((oa == net::CLAIM_OURS) != (ob == net::CLAIM_OURS), L"Exactly one wins.");
            Assert::IsTrue(oa != net::CLAIM_NONE && ob != net::CLAIM_NONE);
            b.Leave(); a.Leave(); host.Leave();
        }

        TEST_METHOD(RestPoseRoundTripsAndRefusesGarbage)
        {
            net::ObjectPose p;
            p.x = 12.5f; p.y = -3.0f; p.z = 400.25f; p.yaw = 1.5f;
            net::WorldStateValue v;
            Assert::IsTrue(p.Encode(v));
            Assert::AreEqual((int)net::ObjectPose::SIZE, (int)v.len);
            net::ObjectPose back;
            Assert::IsTrue(back.Decode(v));
            Assert::AreEqual(400.25f, back.z);
            Assert::AreEqual(1.5f, back.yaw);
            v.len = 3;
            Assert::IsFalse(back.Decode(v), L"Wrong size.");
            p.x = std::numeric_limits<float>::quiet_NaN();
            p.Encode(v);
            Assert::IsFalse(back.Decode(v), L"Never a NaN pose.");
        }

        TEST_METHOD(SecondPusherFollowsTheFirst)
        {
            // Two players push the same parked car: both announce an entity bound
            // to it; the server keeps the first owner and the other follows its
            // stream with its own copy of the car (spawn sink gets the bound id).
            for (MigGhost* g : g_migGhosts) delete g;
            g_migGhosts.clear();
            NetEntityManager m;
            std::vector<net::WorldStateOp> ops;
            m.SetSenders([&](const net::WorldStateOp& op) { ops.push_back(op); return true; },
                         NetEntityManager::SendStatesFn());
            NetEntityManager::Sink s = { &MigSpawn, &MigMove, &MigDespawn, &MigAdopt, &MigHit };
            m.SetSink(s);
            m.OnWelcomed(2);

            const uint64_t carId = 0x0000777700000003ull; // the parked car's world object id
            int car = 0;
            net::NetEntitySpawn sp; sp.kind = net::NEK_PARKED_CAR; strcpy(sp.model, "compactA");
            m.AddLocal(&car, sp, carId);
            Assert::AreEqual((size_t)1, ops.size());
            Assert::AreEqual(carId, ops[0].key.object, L"Announced under the car's own id.");
            Assert::IsTrue(m.IsLocal(&car));

            net::WorldStateEntry e;
            e.key = ops[0].key; e.owner = 7; e.policy = net::WSP_OWNER;
            sp.Encode(e.value);
            m.OnAuthoritative(e, net::WSR_REJECT);
            Assert::IsFalse(m.IsLocal(&car), L"Lost: not streamed by us.");
            Assert::AreEqual((size_t)1, g_migGhosts.size(), L"Follows the winner.");
            Assert::AreEqual(carId, g_migGhosts[0]->id);
            m.OnMatchEnd();
        }
    };
}
