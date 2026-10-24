# HAR-Engine — Networking Layer Specification

> [🇷🇺 Russian version](SPEC.ru.md)

> **Status:** living document. Update and consult it during every development
> cycle, so you never create duplicate functionality. Mark implemented features
> and describe new function relationships as they appear.

> The project evolves as an **independent game engine**; the code base is the
> modern community rebuild of _The Simpsons: Hit & Run_ taken as a starting point.

This file describes the engine's networking layer and how its parts interact.
It is intended both for another AI/developer studying the project and for the
assistant itself, so it doesn't get lost. World terminology ("player", "vehicle",
"mission") refers to content the engine loads through its world interfaces, not
to any specific game.

---

## 1. Project map (network area)

The network layer is split into independent layers: the reusable engine layer
(`src/game/netengine/`), the game layer (`src/game/network/`) and the dedicated
server product (`src/server/`). The legacy monolith `NetworkServer`/`NetworkClient`
has been fully removed.

### Engine layer `src/game/netengine/` (reusable, world-agnostic)

| File | Role |
|------|------|
| `Packet.h` | Wire protocol: `PacketId`, `PROTOCOL_VERSION`/`PROTOCOL_MAGIC`, refusal reasons, ENet channels + per-id delivery (`ChannelOf`), bounds-checked `Writer`/`Reader` |
| `PacketRegistry.{h,cpp}` | Packet registry (id → handler) |
| `ServerEndpoint.{h,cpp}` | Pure ENet host: `PeerId`s, framing, liveness sweep, `KickPeer` (drains queued packets first), server clock `NowMs`, PONG with server time, per-peer RTT |
| `ClientEndpoint.{h,cpp}` | Pure ENet client: framing, time-based heartbeat, RTT + server-clock estimate (`ClockSync`) |
| `NetTime.h` | `ClockSync`, `SnapshotBuffer<T>` (jitter buffer), `TokenBucket` |
| `ServerCore.{h,cpp}` | **The authoritative server** (one implementation for the hosting game and the dedicated server): handshake, admission, world state, ledger, relays, validation, rate limits, session options, ownership migration expiry |
| `Session.{h,cpp}` | A match participant: always a client; when hosting it also runs an embedded `ServerCore` and joins it through loopback |
| `WorldState.{h,cpp}` | Replicated world table / mirror / codecs, ownership transfer (§17, §18.5) |
| `Ledger.{h,cpp}` | Server-authoritative counters (§17.3) |
| `GamePlayerBridge.{h,cpp}` | Game adapter: ties `PlayerNetworkState` to the session's opaque blobs |
| `WorldInterfaces.h` | `ILocalStateProvider`/`IRemoteStateSink`/`IStateValidator`/`IMissionStateProvider` |
| `ENetInit.{h,cpp}` | RAII reference-counting for ENet |

### Game layer `src/game/network/`

| File | Role |
|------|------|
| `NetworkPacket.h` | Little-endian primitives (`net::Write*/Read*`) |
| `PlayerNetworkState.{h,cpp}` | Player state struct + serialization (§3) |
| `GameServer.{h,cpp}` | The game's server rules applied to a `ServerCore` (economy, movement validation, Lua mods, PvP) — used by the hosting game AND the dedicated server |
| `PlayerMovementValidator.{h,cpp}` | Server-side movement plausibility (§18.3) |
| `NetworkMatchManager.{h,cpp}` | Orchestrator for gameplay (§10) |
| `RemotePlayerManager.{h,cpp}` | Remote-player registry (state + server sample time) |
| `RemoteStateTrack.h` | Snapshot interpolation of one remote player (§11) |
| `RemotePlayerWorldView.{h,cpp}` | Remote on-foot players in the world |
| `RemoteVehicleWorldView.{h,cpp}` | Ghost machines of remote drivers |
| `NetWorldRegistry`, `NetEntityManager`, `PlayerEconomy`, `NetWorldHooks`, `WorldObjectId.h`, `EconomyConfig.h` | Replicated world (§17, §18) |
| `ServerModManager.{h,cpp}` | Server-side Lua mods |
| `MissionSyncManager`/`MissionSyncState` | Mission progress (co-op) |
| `NetworkConsoleCommands.{h,cpp}` | Console commands `host`/`join`/`match` |

### Dedicated server `src/server/` (product)

`HarServer.vcxproj` → `har_server.exe` (§8).

In-game integration: `src/game/contexts/gameplay/gameplaycontext.cpp`
(providers, sinks, calls in the game loop) and
`src/game/main/commandlineoptions.{h,cpp}` (CLI auto-start).

---

## 2. Network model and transport

- **Transport:** ENet (vcpkg `enet`), three channels (`Packet.h`):
  - `CH_RELIABLE` (0) — reliable ordered: handshake, world state, ledger,
    events, user data;
  - `CH_STREAM` (1) — unreliable sequenced: latest-state streams
    (`PID_PLAYER_STATE`, `PID_MISSION_STATE`, `PID_ENTITY_STATE`,
    `PID_USER_STREAM`). A lost stream packet is replaced by the next one
    instead of stalling everything behind a retransmit; an older packet that
    arrives late is dropped by ENet's sequencing;
  - `CH_TIME` (2) — unreliable: `PID_PING`/`PID_PONG` (a reliable retransmit
    would inflate the RTT sample).
  The channel is chosen by the packet id in one place (`ChannelOf`); both
  endpoints use it.
- **Authoritative server**, never P2P. The server admits clients, assigns
  `PeerId`s, validates, relays and arbitrates; clients predict and the server
  decides (§17).
- `MAX_PACKET_PAYLOAD = 512` bytes; framing `[u16 id][u16 size][payload]`.
- Liveness: the client pings every 500 ms (`SetPingIntervalMs`); the server drops a
  peer silent for 30 s (`SetClientTimeoutMs`).

### Packet ids (`Packet.h`, `PROTOCOL_VERSION = 3`)

| Id | Name | Dir | Purpose |
|----|------|-----|---------|
| 0x00 | `HELLO` | C→S | `u32 magic, u16 protocol, u32 game version, u8 n, password` |
| 0x01 | `WELCOME` | S→C | `u16 peer id, u32 server time, u32 session options` |
| 0x03 | `REFUSE` | S→C | `u8 reason, u16 server protocol`, then disconnect |
| 0x04 | `PLAYER_LIST` | S→C | admitted peers |
| 0x05/0x06 | `PING`/`PONG` | C↔S | `{client time}` / `{echo, server time}` |
| 0x07 | `WORLD_SEED` | S→C | `u32` |
| 0x08 | `SESSION_INFO` | S→C | session options changed |
| 0x09 | `STATE_CORRECTION` | S→C | validator refused our state: snap to this blob |
| 0x10–0x15 | `WS_*` | | replicated world (§17.1) |
| 0x20/0x21 | `LEDGER_*` | | counters (§17.3) |
| 0x100 | `PLAYER_STATE` | C→S→C | player stream (timestamped) |
| 0x102 | `MISSION_STATE` | C→S→C | mission progress stream |
| 0x103 | `ENTITY_STATE` | C→S→C | owner-checked entity stream |
| 0x104 | `ENTITY_EVENT` | C→S→owner | hit on an entity (§18.6) |
| 0x105 | `PLAYER_EVENT` | C→S→victim | PvP hit (§18.7) |
| 0x8000/0x8001 | `USER`/`USER_STREAM` | C→S→C | opaque mod data, reliable / stream |

Stream framing: client → server `[u32 sample server time][payload]`; server →
others `[u16 sender][u32 time][payload]`. The server clamps the time into
`[now − 2000 ms, now]` (a client cannot claim the future).

---

## 3. Player state `PlayerNetworkState`

Exact byte layout (field order matters for serialization):

```
struct PlayerNetworkState {
    float x, y, z;            // position
    float rotY;               // rotation around vertical
    char  vehicleModel[16];   // vehicle model name ("" if on foot)
    int32 netVehicleId;       // stable network id of the vehicle
    float vehicleSpeed;       // vehicle speed (km/h)
    uint8 inVehicle;          // 1 = in a vehicle
    uint8 flags;              // FLAG_JUMPING | FLAG_IN_AIR | FLAG_LOCO_TURBO
    // --- continuous locomotion (on foot) ---
    uint16 motionAngle;       // desired motion direction, rad*1000
    uint16 facingAngle;       // desired facing, rad*1000
    uint8  speedFactor;       // 0..255 fraction of max speed
    uint8  characterState;    // CharacterAi::CharacterState (LOCO etc.)
    // --- one-shot action ---
    uint8  actionEvent;       // [seq:3 | ACTION_* id:5] latest action; 0 = none yet
    // --- kinematic vehicle mirror (inVehicle) ---
    float  vehYaw;            // machine heading, rad
    uint8  vehSteer;          // signed steer -128..127 -> -45..+45 deg
    uint16 vehSpeedKmh;       // speed, km/h
    uint8  vehState;          // VS_NORMAL / VS_SLIP / VS_EBRAKE_SLIP
    uint8  vehFlags;          // VEH_FLAG_BRAKE | VEH_FLAG_REVERSE | VEH_FLAG_DESTROYED
    uint8  vehDamage;         // machine damage 0 (intact) .. 255 (wrecked)
    uint8  teleportSeq;       // bumped on every deliberate position discontinuity
};
```

- Wire format (protocol 3, `Serialize` returns the size, `Deserialize(data,
  size)` refuses a truncated / oversized record or an unknown section bit). Only
  what the player is doing is sent:
  ```
  u8  sections            SEC_LOCO 0x01 (on foot) | SEC_VEHICLE 0x02 (in a machine)
  i24 x, y, z             centimetres (+-83.88 km, clamped; NaN -> 0)
  u16 rotY                angle 2pi/65536, decodes into (-pi, pi]
  u8  flags | u8 characterState | u8 actionEvent | u8 teleportSeq
  [SEC_LOCO]    u16 motionAngle | u16 facingAngle | u8 speedFactor
  [SEC_VEHICLE] i32 netVehicleId | u16 vehYaw | u8 vehSteer | u16 vehSpeedKmh
                | u8 vehState | u8 vehFlags | u8 vehDamage
  ```
  On foot 21 bytes, in a machine 28 (`MAX_SERIALIZED_SIZE`); protocol 2 sent a
  fixed 60. Fields of an absent block decode as 0; `vehicleSpeed` is restored
  from `vehSpeedKmh` (the sender filled both from `GetSpeedKmh`). The shared
  fixed-point helpers are `netengine/Quantize.h` (`WriteCentimetres24`,
  `EncodeAngle16`, ...).
- In a vehicle the locomotion/action fields are zeroed; on foot the vehicle fields are.
- `actionEvent` comes from `net::LocalActionLatch` (`LocalActionLatch.h`): the game's
  action sites for the local avatar call `Report(id)`; the latch holds the latest
  action with a 3-bit sequence (`MakeActionEvent`/`ActionId`). The receiver fires on
  a byte change. Sources: `state.cpp` (DODGE, CRINGE, ATTACK, CELEB_SMALL/BIG, FLAIL
  in `InSim::Enter`), `action.cpp` (SLAM, JUMPKICK, GETUP). Jumps ride
  `FLAG_JUMPING`. `ACTION_KD`/`ACTION_SURF` are reserved.
- `vehDamage` = `(1 − Vehicle::GetHealthFraction()) · 255`; receivers show it on the
  ghost machine (`Vehicle::ApplyNetworkDamage`).
- `teleportSeq`: the game teleports the avatar from many places, so the provider
  detects it (`TrackLocalTeleport` in `gameplaycontext.cpp`): a jump between two
  samples that no vehicle (110 m/s) or runner (20 m/s) can make, plus 25 m. A
  server correction is not counted (the server already knows that pose). The server
  lets a jump through only together with a new value, rate-limited (§18.3).
- `vehicleModel` is not on the wire: it is the `WOC_VEHICLE` presence record
  (§18.11), filled back in on receipt; with `netVehicleId` it goes to
  `RemoteVehicleWorldView`, so the receiver spawns a ghost of the same model
  (until the record arrives the state counts as on foot for the vehicle view).

---

## 4. Server: `net::ServerCore`

One authoritative server implementation (`netengine/ServerCore.h`). The hosting
game runs it inside its `Session` (embedded) and joins it through loopback; the
dedicated server product (§8) runs the same class standalone. Hence a hosted
match and a dedicated-server match behave identically.

### Handshake and admission
1. ENet connect → `ServerEndpoint` assigns a `PeerId` (from 1). The peer is NOT
   admitted: nothing but `PID_HELLO` is accepted from it, and it receives nothing
   about the world (all world/roster broadcasts go to admitted peers only).
2. `PID_HELLO` is checked in order: magic + `PROTOCOL_VERSION` → game version
   (`SetGameVersion`, 0 = unchecked) → password (`SetPassword`, empty = open
   session) → slot cap (`SetMaxPeers`, the host's loopback included) → admission
   filter (`SetAdmissionFilter`, e.g. a Lua mod's `on_join`). Any failure sends
   `PID_REFUSE{reason}` (`REFUSE_PROTOCOL`, `_GAME_VERSION`, `_PASSWORD`, `_FULL`,
   `_DENIED`) and disconnects after the refusal is delivered.
3. A peer that does not send a valid HELLO within `SetHandshakeTimeoutMs` (5 s) is
   refused with `REFUSE_HANDSHAKE`.
4. Admission: `WELCOME{peer, server time, options}` + `WORLD_SEED` +
   `PLAYER_LIST` to everyone + the world snapshot (reliable ordered channel: the
   whole world arrives before any later change).
5. The ENet transport capacity is the slot cap + 4, so a client over the cap is
   told `REFUSE_FULL` instead of timing out.

### Rate limits (`RateLimitConfig`, `SetRateLimits`)
Per admitted peer, token buckets per class: streams (90/s, burst 180), requests
(world state / ledger / events: 240/s, burst 600 — a joiner flushes its queue at
once), reliable user packets (60/s, burst 120) and bytes (96 KiB/s, burst 192
KiB). A packet over budget is dropped; 300 drops within 10 s →
`REFUSE_FLOOD` and kick. `GetStats()` counts refusals, drops, validator refusals,
corrections and flood kicks.

### Relay and validation
- `PID_PLAYER_STATE` passes `IStateValidator::ValidatePeerState(from, blob,
  serverNow, correction)` (§18.3): `SV_ACCEPT` relays, `SV_DROP` drops,
  `SV_CORRECT` drops and sends `PID_STATE_CORRECTION` with the blob to snap to.
- Streams are relayed timestamped (§2); entity streams only for records the
  sender owns; entity events go to the owner; player events to the victim with
  PvP on.
- Departure: owned entities are erased or released (§18.5), balances dropped, the
  validator forgets the peer, `PLAYER_LIST` is broadcast.

### Observers
`SetJoinLeaveFn` (server-side join/leave), `SetWorldChangeObserver` (every
accepted change/erasure), `SetUserPacketObserver`.

---

## 5. Client: `net::Session`

- `Join(ip, port)` → `ClientEndpoint`; once the ENet connection is up, `PID_HELLO`
  goes out with `SetGameVersion` and `SetPassword` (a hosting session presents its
  own server's password).
- `GetRefusal()` — why the server refused us (the session has then left the
  match). `IsConnectionLost()` — an established connection dropped without a
  refusal. A refusal is applied only after the transport's service loop returns
  (the endpoint is never destroyed inside its own callback).
- Server clock: seeded by WELCOME, refined by every PONG (`ClockSync`: the
  lowest-RTT sample of the last 8 gives the offset; the RTT is smoothed).
  `ServerTimeMs()`, `GetRttMs()`, `IsClockSynced()`.
- Join/leave: derived from the `PLAYER_LIST` diff on every client (host loopback
  and joiners alike) → `SetJoinLeaveFn` and `IRemoteStateSink::OnPeerLeft`.
  `Leave()` reports the remaining peers as gone; the destructor reports nothing
  (the sinks may already be destroyed).
- Streams: local state every `SetStateSendIntervalMs` (100 ms), mission progress
  every `SetMissionSendIntervalMs` (300 ms), time-based.
- Requests (world state, ledger, events, reliable user data) wait in an outbox
  (≤ 256, order kept) until WELCOME.
- Session options: `GetSessionOptions()` / `SetSessionOptionsFn` (bit `SO_PVP`);
  hosting: `SetSessionOptions`.
- Events: `SendEntityEvent`/`SetEntityEventSink`, `SendPlayerEvent`/
  `SetPlayerEventSink`; user data `SendCustomEvent(data, size, stream)`.
- `SetStateCorrectionSink` receives server corrections.

---

## 6. Vehicle sync (A2)

**Key reuse rule:** transport data is carried **inside `PLAYER_STATE`**; the
separate `PACKET_VEHICLE_STATE` is declared but not used. Do not duplicate it.

### Sending side (`GameplayLocalStateProvider` in gameplaycontext.cpp)
- Reads `Avatar`: position, rotation, `IsInCar()`.
- If in a vehicle: `vehicleModel = vehicle->GetName()`,
  `netVehicleId = mVehicleCentralIndex` (or pointer hash),
  `vehicleSpeed = vehicle->GetSpeedKmh()`.
- If on foot: `vehicleModel[0]=0`, `netVehicleId=0`, `inVehicle=0`.

### Receiving side (`RemoteVehicleWorldView`)
- Sink abstraction (spawn/move/despawn), like `RemotePlayerWorldView`,
  unit-testable. Real world work is done by the gameplay context.
- `SyncDriver(playerId, state)`:
  - on foot and no spawn → do not spawn;
  - entered a vehicle → spawn a "ghost" of the model;
  - model/id changed → despawn old + spawn new;
  - exited → despawn.
- `Prune(players)` — despawn vehicles of drivers missing from the authoritative set.
- `SyncDriver(playerId, state, serverTime)` records a timestamped sample;
  `Update` renders the snapshot-interpolated pose (§11), `vehYaw` over the
  shortest arc; steer/speed/state/flags/damage come from the nearer sample.
- A ghost the world could not create (no free collision area) is retried on the
  next sample.

### Sink in gameplay context (`RemoteVehicleSpawn/Move/Despawn`)
- Spawn via `VehicleCentral::AddNetworkGhostVehicle(model)` — a "network
  ghost": in the scene and collision (renders and blocks) but **not** in the
  active list and with no type any manager sweeps (traffic, parked cars,
  mission reset, husk). Nothing but the network layer can free it.
- Move — kinematic mirror: `SetTransform` (position + `vehYaw`), then speed is
  re-applied (SetTransform zeroes it, otherwise wheels don't spin), steer,
  brake/reverse lights.
- Despawn — `RemoveNetworkGhostVehicle` (its final `Release()` self-deletes the
  object). Do **not** `delete` — that double free crashed the host with two
  remote drivers. A ghost removed while its vehicle script (`.con`) is still
  loading keeps one reference until construction finishes
  (`ReleaseDeferredNetworkGhosts`): the script's console hooks write into the
  vehicle under construction (a freed ghost there was a use-after-free).
- Ghosts have their own collision-area budget
  (`WorldPhysicsManager::NETWORK_GHOST_VEHICLES = 16`, sim
  `CollisionAreaListSize = 37 + 16`); `AddNetworkGhostVehicle` returns NULL when
  none is free instead of the release assert.
- Damage: `Vehicle::ApplyNetworkDamage(1 − vehDamage/255)`; a ghost is never
  damaged locally (`Vehicle::mNetworkGhost`, §18.6).

---

## 7. Players: list and notifications (C8/C9)

- `net::Session` on the receiving side rebuilds the authoritative peer roster
  (`GetPeerRoster`) on every `PLAYER_LIST`; join/leave events are delivered via
  `SetJoinLeaveFn(peerId, joined)` at the session level.
- The game layer drains accumulated `JoinLeaveEvent{clientId, joined}` through
  `NetworkMatchManager::DrainJoinLeaveEvents` for UI/console.
- In the game loop `gameplaycontext` prints "net: player N connected /
  disconnected" to the console.
- Console command `match status` prints remote player composition via
  `ForEachPlayer`.
- `KeepOnly` in `RemotePlayerManager` applies the authoritative list.

---

## 8. Dedicated server (product)

`src/server/HarServer.vcxproj` → `har_server.exe`: the bare `net::ServerCore` with
the game's rules (`GameServer`: economy, movement validation, PvP, server Lua
mods) and no game, window, assets or loopback player — the server takes no slot.
It is the same server a hosting game embeds.

```
har_server [port] [--password <pw>] [--max <players>] [--mod <lua>]...
           [--pvp] [--cheats] [--game-version <n>] [--seed <n>]
           [--no-interest] [--stats <seconds>] [--log <file>] [--verbose]
```
- `--stats` / `--no-interest`: traffic report and interest management off (§18.11).
- Default port 3737; no password = open session; `--max` = player slots.
- `--mod` may repeat (one `ServerModManager` per script).
- `--verbose` logs every world-state change; refusals / flood kicks / movement
  corrections are logged from `ServerCore::GetStats()`.
- Ctrl+C / SIGTERM stop it cleanly. The build copies `lua.dll` next to the exe.

`src/tests/mp_standalone` is now only a headless TEST client (`join`, `bots`,
`genui`): `join` joins a hosting game or `har_server`, sends a fake moving state
and prints the remote players and the late-join snapshot it receives; `bots` is
the load test (§18.11).

### Main game CLI (`CommandLineOptions`)
- `HOSTPORT=<port>` — auto-start host.
- `JOINIP=<ip[:port]>` — auto-start join.
- `HOSTPASS=<pw>`, `HOSTSLOTS=<n>`, `JOINPASS=<pw>` — configuration.
- Gameplay options (non-network, off by default): `FREEROAM` (load the level in
  Sunday Drive without a mission), `ALLOWCARFLIP` (auto car flip),
  `CINEMATICJUMPS` (cinematic jump camera), `EXITCARFADE` (legacy fade +
  teleport car exit).
- All these options apply in `GameplayContext::OnStart`, then
  `ClearNetworkOptions()` clears them so the match is not restarted again.

---

## 9. Lua mods (`ServerModManager`)

- One manager per script (`LoadScript(path)`); `GameServer` loads the configured
  list for BOTH kinds of server (hosting game: `NetworkMatchManager::SetHostMods`;
  dedicated: `--mod`).
- Hooks: `on_join(clientId)` → admission filter (false = `REFUSE_DENIED`),
  `on_validate(clientId, x,y,z, speed)` (speed = measured m/s since the last
  accepted state) after the movement rules, `on_leave(clientId)`; events
  `player.joined` / `player.left` through `net.on`.
- Without a script, behavior = built-in authoritative server.
- Test script: `src/tests/test_server_mod.lua`.

---

## 10. Orchestrator `NetworkMatchManager`

Single network entry point from gameplay. Since the migration to the engine layer
this is a **thin bridge**: it holds a `net::Session` + `net::GamePlayerBridge` and
keeps the former public API so consumers (gameplay, console, missions, vehicles)
do not change.

- **Host** plays as a full participant: `StartHost(port)` starts the server and
  immediately connects a loopback client to its own port (`127.0.0.1:port`). So the
  host holds both `m_server` and `m_client` and is visible to other clients (its
  local state is relayed like any joined client's). `HostAndJoinAreMutuallyExclusive`
  still forbids an *explicit* join while a match is running, but the host attaches
  itself via loopback.
- **Join client** — only `m_client` (`JoinHost`).

- `StartHost(port)` / `JoinHost(ip, port)` / `Leave()` — match lifecycle
  (delegates to `net::Session`).
- `SetHostConfig(password, maxPlayers)` — config; the host password is mirrored
  onto the loopback client so the host passes the same authentication gate.
- `SetLocalStateProvider(fn)` — local-state provider (`GamePlayerBridge`).
- `GetRemotePlayers()` — registry **from the client side** (for both host and
  join-client), because the client perspective excludes the player itself
  (`selfId`) from the remote set; the raw server registry would include the host
  to itself.
- `DrainJoinLeaveEvents(out)` — join/leave notifications.
- `Update()` — drives `net::Session::Update()`; when the match ends on its own
  (refused / connection lost) it leaves and resets the world services;
  `GetLastRefusal()` / `WasConnectionLost()` tell why.
- Hosting attaches `GameServer` to the session's embedded core
  (`SetHostConfig`, `SetHostPvp`, `SetHostMods`, `SetAllowEconomyCheats`).
- PvP and corrections: `SetStateCorrectionHandler`, `SetPlayerHitHandler`,
  `SetRemoteLookups` (ghost machine / puppet → peer), `ReportRemoteVehicleHit`,
  `ReportRemotePlayerHit`, `IsPvpEnabled`, `IsRemotePlayerNear`.

---

## 11. Interpolated visualization (`RemotePlayerWorldView`)

- Sink: `spawn`/`move`/`jump`/`action`/`despawn` decoupled from the world (testable).
- **Snapshot interpolation** (`RemoteStateTrack.h`, `net::SnapshotBuffer`): every
  relayed state carries the server time its sender sampled it at; the view keeps a
  short history per player and renders at `serverNow − interpolationDelay`
  (100 ms = two periods of the 20 Hz player stream), which normally lies between two received samples:
  the player follows the sender's real path (jump arcs included) regardless of
  packet jitter. A late packet is covered by extrapolating the last segment, at
  most `maxExtrapolationMs` (250 ms). Frame time does not matter.
- `Sync(players)` records new samples (a repeated packet is ignored) and
  spawns/despawns by composition. `Update` moves every player to its interpolated
  pose and fires the edges (jump rising edge, a changed `actionEvent` byte) of the
  RENDERED state — a jump or clip starts when the shown pose reaches it, not one
  interpolation delay early. The first shown state only primes the edges (no
  replay on spawn / reappearance).
- The move sink also receives the pose's velocity (`RemoteMotion`, from the
  bracketing samples). In game (`RemotePlayerMove`) the puppet is placed exactly at
  the interpolated pose every frame (after the world physics update — the last
  write before rendering) and the locomotion driver gets the MEASURED horizontal
  speed and direction, so the walk/run cycle matches the real motion and the
  animation's own advance lands on the next pose. The old scheme (locomotion at
  `speedFactor · maxSpeed`, position snapped back after drifting 1 m) was the
  rubber-banding.
- The sender's `rotY` is the facing angle (`Character::GetFacingDir`); it used to
  carry `heading.y` (the vertical component of a direction vector, ~0).
- Continuous fields (position, `rotY` over the shortest arc, `vehYaw`) are
  blended; discrete ones (flags, locomotion inputs, states) come from the nearer
  sample. Locomotion fields drive the puppet's `LocomotionDriver` in game.
- Untimed input (time 0: no clock) shows the newest state.
- `SetTiming(Timing{clock, delay, extrapolation})`; the game passes
  `Session::ServerTimeMs`. The ghost-machine view (§6) uses the same track.
- The `action` sink receives the bare id; a held action is not replayed on spawn;
  `RemotePlayerAction` replays the clip visually.

---

## 12. Mission sync (A3, co-op)

Missions become **shared only when players play through them together**, not
forcibly for everyone. Mechanic — mission-progress relay, like `PLAYER_STATE`:

- **State:** `MissionSyncState` (mission/stage/objective + `inMission` flag),
  serialized like `PlayerNetworkState`.
- **Local:** `GameplayMissionStateProvider` in `gameplaycontext.cpp` reads the
  current mission via `GameplayManager::GetCurrentMission()` /
  `GetCurrentStageIndex()` and sends it over the network (client → server →
  others).
- **Server:** accepts `PACKET_MISSION_STATE`, stores per client in
  `m_missionStates`, relays to others (no gate validation — mission progress is
  decided by the host).
- **Client:** `GetRemoteMissionStates()` returns `map<clientId, MissionSyncState>`
  for UI "who's in which mission".
- `MissionSyncManager` aggregates local + remote progress, testable.
- **Engine channel:** a dedicated `PID_MISSION_STATE` in `net::Session`.
  Provider — `IMissionStateProvider` (`WorldInterfaces.h`), sink —
  `SetMissionStateSink`. Sent on its own slower cadence (`SetMissionSendEvery`,
  default every 18 frames ≈ 3.3 Hz). The server relays `[sender][payload]` to
  everyone except the sender, no validator.
- `NetworkMatchManager::AttachMissionChannel()` wires `GameMissionAdapter` to the
  session; remote progress via `GetRemoteMissionStates()` /
  `GetRemoteMissionState(peerId, out)`. Test — `MissionStateRelaysThroughDedicatedChannel`.

### 12.1 World state

The `WorldEvent` channel over `PID_USER` was removed. Shared world objects, the
economy and traffic are synchronized server-authoritatively — see **§17**.
`PID_USER` remains for mods and custom events only.

## 13. Game-loop integration points (`gameplaycontext.cpp`)

Order in `GameplayContext::OnUpdate` inside a match:
1. `pMatch->Update()` — network (and sending mission state via the message provider).
2. `GetRemotePlayerWorldView()->Sync(...)` → `Update(elapsedTime)` — on-foot.
3. `ForEachPlayer` → `RemoteVehicleWorldView::SyncDriver` → `Prune` → `Update` — vehicles.
4. `DrainJoinLeaveEvents` → console messages.

In `OnStart`: CLI auto-start of the match, `SetSink` for both WorldViews,
`SetMissionStateProvider` for local mission progress.

---

## 13. Compatibility: tests (project rule)

- Every changed capability must have unit/integration tests
  (`src/tests/network_tests.cpp`; project Tests.vcxproj, package `Tests.dll`).
- Run: `vstest.console.exe` against `Tests.dll`. Current state — 42/42 green.
- Full game build: `MSBuild.exe src/srr2/SRR2.vcxproj
  /p:Configuration=DebugWindows /p:Platform=x64
  /p:WindowsTargetPlatformVersion=10.0.22621.0`.

---

## 14. Entering multiplayer from the main menu (C7)

**Integration without generating a new frontend.p3d** — the "MULTIPLAYER" item
is added by code into the existing main menu (`CGuiScreenMainMenu`).

- `guiscreenmainmenu.h`: `MAIN_MENU_MULTIPLAYER` inserted into `eMainMenuSelection`
  (now the item between `MAIN_MENU_MINI_GAME` and `QUIT_GAME`). The label string
  is assigned in `InitMenu()` via `mainMenuText->SetString(MAIN_MENU_MULTIPLAYER, "MULTIPLAYER")`
  (TextBible has no localized string for the new slot).
- Item selection (`MAIN_MENU_MULTIPLAYER` in `GUI_MSG_MENU_SELECTION_MADE`) →
  `OnMultiplayerSelected()` → `StartMultiplayerOverlay()`.
- In the overlay `GUI_MSG_CONTROLLER_UP/DOWN` toggle `Host`/`Join`,
  `GUI_MSG_CONTROLLER_SELECT` confirms (`OnMpOverlayConfirm` → Host/Join).

### IP input via `TextInputHook`
- `src/game/presentation/gui/utility/textinputhook.{h,cpp}` — global text-input
  hook: `SetActive(OnChar, OnBackspace)`/`ClearActive()`, `HasActiveConsumer()`.
  Only one active consumer.
- In `win32platform.cpp` in the SDL loop inserted: `SDL_TEXTINPUT` →
  `TextInputHook::ReportText(...)`, `SDLK_BACKSPACE` → `ReportBackspace()`.
- The menu screen registers static handlers `OnMpOverlayChar`/`OnMpOverlayBackspace`,
  buffer `m_mpIpBuffer[64]`; `UpdateMpOverlayDisplay()` draws the Host/Join + IP prompt.

### Entering MP gameplay (host/join) — important pitfalls
`StartMultiplayerHost`/`StartMultiplayerJoin` must fully repeat the "New Game"
path, otherwise the world/character/controller are not ready:
1. `NetworkMatchManager::StartHost(port)` / `JoinHost(ip, port)` — network (before
   entering gameplay; server/client starts immediately).
2. `GetInputManager()->RegisterControllerID(0, 0)` + `SetPrimaryController(0)` —
   without this `GetControllerIDforPlayer(0)` == -1 → crash in
   `guimanageringame.cpp` (gameplay entry) and no character control.
3. `if (IsGameLoaded()) ResetGame() + ClearRewards()` — reset the loaded save,
   otherwise on game exit an assert in `simutility.cpp:153`
   (SimEnvironment not unloaded due to stray save ph-objects) and the
   interaction key does not work.
4. `GetCharacterSheetManager()->SetCurrentMission(L1, M1)` — otherwise the world
   loads without spawning the playable character (no control after cutscene).
5. Enter via standard `GUI_MSG_QUIT_FRONTEND, 1` (it sets up `GameplayManager`);
   do NOT use direct `SetContext(CONTEXT_LOADING_GAMEPLAY)` — in that context
   `GetGameplayManager()` is still NULL → AV.

### Controller out-of-range guards (degradation)
- `humanvehiclecontroller.cpp`: `GetController(mControllerId)` in
  `GetSteering`/`Update` wrapped in `if (mControllerId >= 0 && < MaxControllers)`.
- `guimanageringame.cpp` (connection poll): guarded `controllerID != -1`.
- These `GetController` calls no longer trigger
  `rAssert(controllerIndex < MaxControllers)` in `inputmanager.cpp`.

### Crash handler (`win32main.cpp`)
- `SetUnhandledExceptionFilter(CrashHandler)` + hooks `abort`/`terminate`/
  pure-virtual (`_set_purecall_handler`, `set_terminate`, `signal(SIGABRT)`).
- Writes a symbolic stack (DbgHelp) to `crash.log` in the working folder, not
  relying on an error window. `WriteCrashDump(reason)` — available for targeted
  diagnostics (temporary call in `GetController` on out-of-range index).

### Known shortcoming
The IP-input overlay is a separate visible three-line prompt
(`MULTIPLAYER HOST` / `Address: IP` / `[Enter=... Esc=back]`) drawn on the hidden
`MainMenu` page text (set `SetBoundingBoxSize`/`SetPositionOfCenter`
and `SetIndex(0)`; `UpdateMpOverlayDisplay` writes via `SetString(0, buf)`).
The menu items are not touched, so navigation does not break. The overlay opens
via `GUI_MSG_MENU_SELECTION_MADE`, not via SELECT in the handler, so
`m_mpOverlayIgnoreFirstSelect` was removed — Enter confirms on the first press.
Empty IP as Host = default port 3737; for Join with an empty address, join does
not start (returns seamlessly). Visually this is still not a dialog with an input
field but a prompt above the menu — further work in TODO.

---

## 15. Plan (TODO, we'll return)

- **C7 (UI lobby, improvement)** — make a separate visible IP/port input field
  in the Host/Join overlay (currently a prompt inside the menu item text).
  Possibly with our own screens after a .p3d generator / UI layer.
- **MP stability — verified end-to-end (2026-10, after the net::* migration).**
  - **Network over real sockets confirmed.** The standalone harness
    `src/tests/mp_standalone` (on `net::Session`/`net::*`) brings up host+join
    as two processes: the server sees `clients in world: 1`, the join gets
    `SERVER READY, my client id = 2`. Extra isolation: an in-game join copy
    connects to a `mp_standalone` server (`selfId=2`) — proving the in-game
    client networking is sound.
  - **Full in-game host↔join (two SRR2 copies in gameplay) does not complete
    on this machine at the OS level, not in the network code:** both processes
    service their ENet sockets (probe markers confirmed polling and receive),
    the server UDP socket is alive on `0.0.0.0:<port>`, the client connect is
    created correctly, but the ENet handshake between two instances does not
    finish even with VPN off and the firewall profiles (Private/Public) off.
    This is an environment limitation of running two headless SRR2 copies, not
    a defect in the `net::` layer.
  - **Manual host/join via the main menu (visible windows)** remains the
    recommended way to check character control; the temporary net-log markers
    were removed after diagnosis, leaving the `net::` layer clean.

### Known issue (POTENTIAL, not confirmed as a code defect)
**Full in-game host↔join between two SRR2 copies does not complete the ENet
handshake.** 2026-10, one machine. Symptoms: both processes service their ENet
sockets (polling and receive confirmed by markers), the UDP server listens on
`0.0.0.0:<port>`, the client `enet_host_connect` creates a peer correctly — yet
the server never accepts an external peer and the join's `selfId` stays 0.
Not helping: VPN off, firewall profiles (Private/Public) off, visible windows
instead of headless, different ports, sequential start. Contrast: the same
`net::` layer works correctly over real sockets in `mp_standalone` (host+join,
server sees peers, join gets `selfId=2`) and an in-game join connects to a
`mp_standalone` server. **Needs a check:** is it a limitation of running two
headless SRR2 copies on this machine, an environment block (Windows Filtering
Platform/driver), or a real `net::` defect — reproduce on a machine without
third-party network stacks/drivers, or on another machine.

**Recorded (not a network bug):** rare one-off flake "world load sticks almost
at the end" entering as host through the menu. Investigation: the network
`NetworkMatchManager::Update` is non-blocking (`enet_host_service` with timeout 0),
called only after the world loads (gameplaycontext.cpp:785), unrelated to the
hang. `skipfe HOSTPORT=3737` (same network start) and the plain start passed
clean over 7+ runs (resp, no crash); `0xc0000005` crashes in the system log were
from old builds. Conclusion: this is an intermittent level-load flake (fixed by
restarting), not a multiplayer issue.

---

### Auto UI driver for tests (opt-in)
`CGuiSystem::Update` (guisystem.cpp) has a built-in **front-end test harness**:
with `SRR2_AUTOMENU=1` it self-presses `SELECT`/`BACK`/`DOWN` on a timer,
logging each step to `mp_dbg.log`. Disabled by default (`getenv` == NULL) and
does not affect gameplay. Purpose: automated menu/screen sweep to surface
crashes without manual testing. Only enable for debugging; needs a warm-up so it
fires only after the front-end is up.

### Crash handler vs rAssert (important)
`CrashHandler` (win32main.cpp) catches access violations (`0xc0000005`) via SEH
and, through hooks, `abort`/`terminate`/pure-virtual call → writes a stack to
`crash.log`. **But `rAssert`/`rAssertMsg` do NOT write crash.log**: when they fire
in Debug, `rDebugAssertFail_Implementation` (radcore raddebug/debug.cpp) calls
`exit(-10)` on Abort — that is neither SEH nor our hooks, so no stack is dumped
(only console text). Gap: rAssert asserts are never dumped. If needed, hook the
Abort branch of the assert to write `crash.log`.

---

## 16. Rule for the future

**Before and during any development, consult this document.** If a feature is
already described, do not recreate it and do not add a duplicate transport/packet
(example: `PACKET_VEHICLE_STATE` is declared, but vehicle data lives in
`PLAYER_STATE` — don't make a second path). Update this file after major changes.

**We are building an engine.** Solutions are fundamental: shared objects are
synchronized through the server with stable deterministic ids; receiver-side
heuristics (matching the "nearest" object by position etc.) are not allowed. Known
limitations are stated explicitly (§17.7), not papered over.

---

## 17. Replicated world (server-authoritative)

Every shared game object is synchronized **through the server** with stable,
deterministic network ids. No receiver-side heuristics (nearest-object matching
etc. is forbidden — see §16). Model: **prediction + arbitration** — the client
applies a change at once, the server decides, everybody gets the result, and a
refused prediction is rolled back.

### 17.1 Engine layer `net::WorldState` (`netengine/WorldState.h`)
- Key `(u64 object, u8 channel)`, value ≤ `WS_MAX_VALUE` (48) bytes, version,
  owner, policy, flags. Cap `WS_MAX_ENTRIES` (65536) per match.
- Write policies (fixed by the entry's creator, cannot be downgraded):
  `WSP_LAST_WRITE` (doors, platforms), `WSP_FIRST_WRITE` (broken/taken — forever;
  the same value is not a conflict), `WSP_OWNER` (only the owner writes/erases).
  `WSF_ERASE_ON_OWNER_LEAVE` erases the entry when the owner disconnects; other
  owned entries are orphaned (owner 0, read-only).
- Packets: `PID_WS_REQUEST` (C→S), `PID_WS_UPDATE`/`PID_WS_ERASE` (S→all; the
  requester matches its seq = confirmation), `PID_WS_REJECT` (S→requester, with the
  current value to roll back to), `PID_WS_SNAPSHOT` (+`_END`) for joiners.
- Server: `WorldStateTable::Apply`. Client: `WorldStateMirror` (confirmed values +
  outstanding predictions). Sink: `Session::SetWorldStateSink(entry, reason,
  origin)`, reasons `WSR_REMOTE/CONFIRM/REJECT/SNAPSHOT/ERASED`.
- **Late-join snapshot:** right after WELCOME the server streams the whole table in
  ≤ 512-byte chunks plus `PID_WS_SNAPSHOT_END` (reliable ordered channel: the world
  arrives before any later change). `Session::SetWorldSyncedFn` /
  `WorldStateMirror::IsSynced()`.
- Requests made before WELCOME (and ledger requests) wait in the session outbox
  (≤ 256, order kept) and go out after WELCOME.

### 17.2 Object identity (`network/WorldObjectId.h`, `NetWorldRegistry`)
- Static object: `id = FNV-1a64(kind, source uid, instance name uid, ordinal)`,
  top bit clear. source = file/sector the object was loaded from; ordinal = index
  of a same-named instance of that source in load order (lowest free one, so a
  reloaded sector gets the same ids). Identical on every client because every
  client loads the same data.
- Dynamic entity: `id = 1<<63 | peerId<<32 | counter`, minted by the owner from its
  server-assigned peer id; peers can never collide.
- `NetWorldRegistry` (singleton `NetworkMatchManager::WorldRegistry()`, works
  outside a match too — objects register while the level loads):
  `Register(kind, source, name, handle, apply)` / `Unregister` / `Request` /
  `OnAuthoritative` / `Pump`. State the server already holds for an object that is
  not loaded yet is applied on registration — deferred to `Pump()` (never inside a
  loader/destructor). While applying (`IsApplying`), `Request` is ignored, so
  mirrored changes are never sent back.
- Channels: `WOC_STATE` (stateprop state), `WOC_BROKEN` (broken/destroyed),
  `WOC_ANIM_DIR` (door/platform animation direction), `WOC_TAKEN` (parked car driven
  away), `WOC_POPULATION` (zone parking plan).
- Integration (`network/NetWorldHooks.h` — thin facade for game code):
  - `AnimDynaPhysLoader` → `StatePropDSG` (`WOK_STATEPROP`); `SetState` →
    `WOC_STATE`; `eRemoveFromWorld` → `WOC_BROKEN` (requested before removal);
    `StatePropDSG::NetApply` applies; `mNetRemoved` prevents a double removal.
  - `AnimCollLoader` → `AnimCollisionEntityDSG` (`WOK_ANIMCOLL`);
    `SetAnimationDirection` → `WOC_ANIM_DIR`.

### 17.3 Economy: counters (`netengine/Ledger.h`, `network/EconomyConfig.h`)
- The engine offers server-authoritative **per-player counters** — the base for any
  economy (currency, tokens, ammo…). A game declares counters and per-reason rules
  (`CounterConfig`): `LR_OPENING` (starting balance, once), `LR_PICKUP_CLAIMED`
  (unique pickup, needs a claim, once per player), `LR_PICKUP`, `LR_REWARD`,
  `LR_SPEND`, `LR_LOSS`, `LR_CHEAT`; delta and balance bounds. Unknown
  counters/reasons are refused (secure default).
- `PID_LEDGER_REQUEST`/`PID_LEDGER_RESULT` (result to the requester only). The host
  and the dedicated server apply the same rules (`economy::MakeCoinConfig`).
- **Coins are per player** but server-validated. Every bank change goes through
  `CoinManager::AdjustBankValue(delta, reason, claim)` (and purchases in
  `RewardsManager`) with the delta actually applied. A world coin carries a claim =
  deterministic id (sector + locator position), so one coin cannot be credited
  twice. `PlayerEconomy` declares the opening balance on join and, whenever none of
  our requests is in flight, converges the local bank to the server balance
  (rolls back refusals, repairs drift). If the server refuses the opening balance,
  economy sync stops for that player (a real balance is never zeroed).

### 17.4 Dynamic entities and traffic (`network/NetEntityManager`)
- Existence, id and ownership live on the server: a `WS_CHANNEL_ENTITY` (0xFF)
  record, `WSP_OWNER` + `WSF_ERASE_ON_OWNER_LEAVE`, value = spawn description (kind,
  model, pose). The owner simulates and streams motion on `PID_ENTITY_STATE`
  (batched); the server relays **only records of entities the sender owns**. Everyone
  else shows a smoothed kinematic ghost. A joiner gets every live entity in the
  snapshot; an owner leaving erases its entities for everyone.
- Traffic: `TrafficManager::AddTraffic` → `AddLocalEntity`, `RemoveTrafficVehicle` →
  `RemoveLocalEntity`. Ghosts use `VehicleCentral::AddNetworkGhostVehicle` and the
  shared `ApplyGhostKinematics` (pose, speed, steer, lights, explosion by flag).
  **Shared density budget:** spawning counts other players' cars nearby
  (`CountRemoteEntitiesNear`), so two players side by side do not double traffic.
- A player's own machine blowing up is `VEH_FLAG_DESTROYED` in `PlayerNetworkState`.
- Ghost rendering, migration, hits, damage, interest management: §18.4–§18.6.

### 17.5 Parked cars (`ParkedCarManager`)
- A zone's placement (which car type on which locator, colour) is a first-write
  `WOC_POPULATION` record of object `(WOK_PARKED_CAR, zone)`. The first client to
  load the zone decides and publishes it; everyone else and joiners use it; a
  client that lost the race re-places from the authoritative plan (zone locators are
  cached in `NetZone`).
- Each placed car is object `(zone, locator+1)`: `WOC_TAKEN` when the local player
  gets in (other clients remove their copy — the player stream mirrors the car from
  then on), `WOC_BROKEN` on explosion (other clients run the same husk swap).

### 17.6 Admission and authorization
- Authorization is **optional** and off by default: without a password the session
  is open (co-op / session games). A password is checked in `PID_HELLO` (§4).
- The server accepts nothing but `PID_HELLO` from a peer before it is admitted and
  sends it nothing about the world.
- Slot cap: `ServerCore::SetMaxPeers` (`Session::SetMaxPeers`,
  `NetworkMatchManager::SetHostConfig`, `har_server --max`); a hosting game's
  loopback takes one slot, the dedicated server none.

### 17.7 Known limitations (open)
- World state changed by the host before the match started (from a save) is unknown
  to the server.
- A released traffic car is adopted only if the claimant has a free pooled car of
  the same model and a road under it; otherwise it expires after the claim window.
- Pushed parked cars, pedestrians and actors: §18.9.

---

## 18. Transport, server core and physics sync (2026-10-25)

### 18.1 One server for host and dedicated
`net::ServerCore` (§4) is the only server implementation. `Session` = client +
optional embedded core; `har_server` = core without a client. `GameServer` (game
rules) attaches to either. `Session::Create/Get/Destroy` (an unused second way to
own a session) was removed.

### 18.2 Channels, handshake, clock
§2, §4, §5: three ENet channels with per-id delivery; `PID_HELLO` with magic,
protocol and game version; explicit `PID_REFUSE` reasons; handshake timeout;
server clock in WELCOME/PONG, `ClockSync` on the client; every stream timestamped
with the server time; snapshot interpolation on receivers (§11).

### 18.3 Movement validation (`PlayerMovementValidator`)
Server-side, on the server's own receive times (a client cannot forge dt). Against
the peer's last ACCEPTED state: horizontal distance ≤ maxSpeed·dt + slack (on foot
20 m/s, vehicle 110 m/s if either state is in a machine, slack 4 m); climbing ≤
maxSpeed·dt + slack, falling ≤ 80 m/s·dt + slack; non-finite / out-of-world
coordinates refused. A jump beyond the limits passes only with a new
`teleportSeq`, at most 3 at once and 0.5/s on average. A refusal answers
`SV_CORRECT` with the last accepted state (at most once per second; states in
between are dropped); the game snaps the avatar (or its car) back
(`GameplayStateCorrection`). The mods' `on_validate` runs last.

### 18.4 Rate limits
§4. Applies to every admitted peer, the host's loopback included.

### 18.5 Ownership migration
`WorldState`: `WSO_RELEASE` (owner → 0, flagged `WSF_CLAIMABLE`, optionally with
a new value = the current pose) and `WSO_CLAIM` (first claim wins; later claims are
rejected with the winner). `WSF_RELEASE_ON_OWNER_LEAVE` releases instead of erasing
when the owner disconnects. Unclaimed entries expire after `SetClaimWindowMs`
(3 s). `NetEntityManager`: when the local simulation drops a migratable entity
(traffic, pedestrians) and another player is within the hand-over radius (150 m),
it is released at its current pose; a player within the claim radius (120 m) with
budget (`CanAdoptFn`) claims it; on success the sink's `adopt` turns the ghost into
a simulated local entity with the same id (`TrafficManager::AdoptNetworkTraffic`:
a free pooled car of the model on the closest lane, at the pose and speed, without
announcing a new entity). Traffic is announced with `WSF_RELEASE_ON_OWNER_LEAVE`.

### 18.6 Hits and damage
- A ghost (`Vehicle::mNetworkGhost`, set by `AddNetworkGhostVehicle`) is never
  damaged locally. In `Vehicle::PostReactToCollision` a collision of OUR vehicle
  with a ghost becomes a `NetEntityHit` {impulse on the ghost (pointing away from
  our car), contact point, damage = normalized magnitude · 255} →
  `networld::ReportGhostHit`: to the entity's owner (`PID_ENTITY_EVENT`, the server
  delivers it to the owner only) or, for a remote player's own car, to that player
  (PvP, §18.7). Ghost–ghost collisions are the owners' business.
- The owner applies it: `Vehicle::ApplyNetworkHit(impulse, damage)` (Δv = impulse
  / mass + `TriggerDamage` + the damage visuals of its type); the result streams
  back.
- Damage is replicated: `NetEntityMotion::damage` and `PlayerNetworkState::
  vehDamage`; ghosts show it with `ApplyNetworkDamage` (only ever more damage).
- `NetEntityMotion` (17 bytes since protocol 3, §18.11) gained `damage` and `state`; flags gained
  `MF_KNOCKED`.
- Interest management: a ghost of a dynamic entity exists only within the
  relevance radius (200 m in game, removed beyond 1.25×; the record is kept); a
  ghost the world cannot create now is retried every frame.

### 18.7 PvP as a session option
`SO_PVP` in the session options (WELCOME / `PID_SESSION_INFO`, changeable live:
`GameServer::SetPvp`, `NetworkMatchManager::SetHostPvp`, `har_server --pvp`). The
server relays `PID_PLAYER_EVENT` to the victim only with PvP on. Kinds
(`NetworkMatchManager::PlayerEventKind`, payload `NetEntityHit`):
`PE_VEHICLE_HIT` (our car hit the victim's ghost machine → the victim's car gets
`ApplyNetworkHit`) and `PE_KNOCKDOWN` (our kick hit the victim's puppet →
`Character::ApplyNetworkKnockdown`: flail + get up via `InSim`, no velocity, like
the original avoids moving the player character). A remote player's puppet is
never knocked locally — with or without PvP (its owner's stream shows the truth).

### 18.8 Deployment of the server product
`HarServer.vcxproj` (Debug/Release x64) copies `lua.dll` after the build;
`src/server/x64/` is ignored by git.

### 18.9 Player context and appearance (`PlayerPresence`)
Slow-changing per-player facts are world-state records of the player's own
object (`MakePlayerObjectId(peer)` = the peer's dynamic id, counter 0;
`WSP_OWNER` + `WSF_ERASE_ON_OWNER_LEAVE`), not part of the 20 Hz stream — reliable,
in the late-join snapshot, published only when they change:
- `WOC_PLAYER_CONTEXT` (6): `u8 level | u64 zone` (interior uid, 0 = outside).
  `RemotePlayerWorldView::SetPresence` / `RemoteVehicleWorldView::SetVisibility`
  show a player (and its machine) only in our context; outside it the state is
  still tracked but there is no puppet. Unknown context = shown.
- `WOC_APPEARANCE` (7): `char model[32] | char anim[16]` = the real model (skin
  included) and animation bank of the local character
  (`CharacterManager::GetModelName/GetAnimName`). The game samples them every frame
  (`PublishLocalPresence`), so ANY appearance change (clothing purchase, mission,
  cheat, a future outfit system) replicates without extra hooks. The remote puppet
  is created with that model and bank (instead of the fixed `homer`/`npd` — the
  player's own clips are available now) and restyled in place on a change
  (sink `restyle` → `CharacterManager::SwapData`). Until the record arrives the
  puppet uses `homer` and is restyled when it does.
- `WOC_VEHICLE` (8): `i32 netVehicleId | char model[16]` — the machine the player
  drives (§18.11).

### 18.11 Traffic: measurement, interest management, compact streams (protocol 3)
Measured with the load test below: 16 players in 4 groups 1 km apart, 2 of them
in an interior, 5 streamed traffic cars each, 20 Hz player stream:

| | server out (ENet wire) | player state | per player received |
|---|---|---|---|
| everything to everyone (`--no-interest`) | 4.4 Mbit/s | 4740 pkt/s | 297 states/s |
| interest management | 0.9 Mbit/s | 1450 pkt/s | 6 / 95 / 110 (interior / group of 3 / of 4) |

The relayed player sample is 31 bytes framed (4 frame + 6 relay header + 21).
- **Accounting.** `ServerEndpoint::GetTraffic()` (`net::TrafficStats`): framed
  packets/bytes per packet id, sent (once per recipient) and received, plus the
  ENet wire totals (UDP payload incl. ENet headers/acks/resends, collected into
  64-bit counters every `Update`). `GetPeerLink(peer)`: RTT, the unreliable
  throttle (0..32) and loss. `ServerCore::GetTraffic()`, `GetPeerLink()`,
  `Stats::samplesFiltered` / `entityRecordsFiltered`.
- **Interest management** (`net::IInterestPolicy`, `ServerCore::SetInterestPolicy`;
  without a policy everything goes to everyone). The core cannot read game
  payloads, so the game decides and the core enforces: `OnPlayerState` (every
  accepted state, after the validator), `PlayerStateIntervalMs(receiver, sender)`
  (0 = every sample, `kNotRelevant` = none, otherwise the minimum spacing — the
  core keeps per receiver the time of the last forwarded sample of each sender,
  with a tenth of the interval as slack), `EntityRelevant(receiver, owner, rec)`
  (each receiver gets its own subset of an owner's batch), `OnPeerLeft`.
  The game's `PlayerInterestPolicy` (installed by `GameServer`, config
  `GameServer::Options::interest`): another context (`WOC_PLAYER_CONTEXT`) →
  nothing; within `nearRadius` 150 m every sample; beyond, one per 200 ms (5 Hz),
  with 25 m hysteresis; an entity record only in the owner's context within
  `entityRadius` 300 m of the receiver (the client hides ghosts beyond 250 m).
  Unknown facts (no context record / no accepted state yet) are relevant.
- **Receivers.** `RemoteStateTrack::Timing::adaptive` (on in game): the delay is
  twice the measured sample spacing, never below `interpolationDelayMs`, slewing
  at 0.25 ms per ms (a far 5 Hz player is shown 400 ms behind, smoothly; a tier
  change speeds playback up/down slightly instead of jumping). A sample more than
  `kGapResetMs` (1 s) after the newest one starts a new history (no slide across
  the time the player was not relevant). `NetEntityManager`: with a clock and a
  relevance radius, a moving entity is shown (and counted by `CountRemoteNear`)
  only while its newest sample is younger than `kStaleMs` (1 s) — the server
  stops forwarding it once it is far; a released entity takes its final pose
  from the release record.
- **Compact streams.** `PlayerNetworkState` (§3: 21/28 bytes instead of 60) and
  `NetEntityMotion` (17 bytes instead of 22: `i24 x,y,z | u16 yaw | u16 speedKmh
  | i8 steer | u8 flags | u8 damage | u8 state`). The vehicle model left the
  stream: presence record `WOC_VEHICLE` (8) `i32 netVehicleId | char model[16]`,
  published by the bridge's local-state observer when it changes and put back
  into each received state by the bridge's decorator (`NetworkMatchManager`),
  when the ids match. `PROTOCOL_VERSION = 3`.
- **One packet for all recipients.** `ServerEndpoint::SendToMany` creates one
  ENet packet and queues it to every recipient (ENet reference-counts it);
  `SendTo`, `Broadcast` and every `ServerCore` fan-out use it.
- **ENet throttle.** ENet drops a share of unreliable packets whenever an RTT
  sample exceeds the previous one plus twice the variance. On a quiet 1-2 ms link
  that is jitter: the load test lost up to 7/8 of the stream to 11-13 peers with
  0 % loss on the wire (the same applies to real games). `net::ConfigureLink`
  (both ends, on connect) configures the throttle to never decelerate; the
  stream rates are bounded by the engine (cadences, interest, per-peer budgets).
- **Load test / diagnostics.** `har_server --stats <s>` prints every N seconds the
  totals (framed and wire, both directions), the samples / records interest kept
  off the wire, the worst link (RTT, throttled peers, loss) and every sent packet
  id (pkt/s, kbit/s, bytes/pkt); `--no-interest` disables the policy for
  comparison. `mp_standalone bots <count> <ip> [port] [--seconds] [--groups]
  [--spread] [--interiors] [--entities]`: that many full engine clients in one
  process (own Session / ENet peer each), circling at 5 m/s, with context
  records and owned streamed entities; prints the states each bot received per
  second.
- **Background launch.** A game started without window focus spent minutes in
  `InputManager::Init`: `radControllerDirectInput` retried `Acquire()` up to
  2.4 s per input device (`DIERR_OTHERAPPHASPRIO`). It now tries once; every poll
  re-acquires the device anyway. Two games + server: both in gameplay in 6-14 s.

### 18.12 Parked-car entry: claim and rollback (P5)
`network/ObjectClaim.h`: an exclusive claim is a record whose value is the
claimant's peer id (`EncodeClaim`, u16 LE) written with `WSP_FIRST_WRITE`; the
server keeps the first claim, every client learns the winner from the
authoritative record (a loser through `WSR_REJECT`, which carries the current
entry); `ClaimOutcomeOf(value, self)` → `CLAIM_OURS` / `CLAIM_THEIRS` /
`CLAIM_NONE`. `networld::LocalPeerId()` gives `self`.
`ParkedCarManager` (parked car `WOC_TAKEN`, previously the constant 1 — two
simultaneous takers both "won"):
- `EVENT_GETINTOVEHICLE_START` of the local player into a networked parked car:
  the current vehicle is remembered (AddRef'd);
- `EVENT_GETINTOVEHICLE_END`: the claim is sent (the entry is predicted);
- verdict `CLAIM_OURS`: the entry stands; `CLAIM_THEIRS`: if the player is in
  (or getting into) the car, the game's own get-out runs (`GetOut`, bailing out at
  speed) and on `EVENT_GETOUTOFVEHICLE_END` the previous current vehicle is
  restored and our copy of the car removed — the winner's machine is mirrored by
  their player stream; otherwise our copy just goes.
- The manager listened to `EVENT_GETINTOVEHICLE_END` twice (the claim was sent
  twice); it now listens to START / END / GETOUT_END once each.

### 18.13 Pushed parked cars (P3)
A parked car is a static world object (registry id, `WOK_PARKED_CAR`); while
someone pushes it, it is also an entity BOUND to that id (`NEK_PARKED_CAR`,
`AddLocal(handle, spawn, boundId)` — the entity record lives under the car's own
id with `WSP_OWNER`, so the first mover owns it and a second one gets
`WSR_REJECT` and follows).
- Who moves it: `Vehicle::PostReactToCollision` — a vehicle-vehicle contact
  (normalized impulse > 0.01) with a vehicle simulated HERE (not a ghost: our car,
  our traffic) calls `ParkedCarManager::OnPushedLocally`, which announces the
  bound entity (`networld::ObjectIdOf`). A push by a ghost is its owner's.
- Owner: the sampler streams it like traffic; `UpdateNetworkPushes` (every frame,
  from `GameplayContext::OnUpdate`) stops when it has been still (< 1 km/h) for
  1 s: the rest pose is written as `WOC_POSE` (6, `net::ObjectPose`, f32 x,y,z,yaw,
  `WSP_LAST_WRITE`) and the entity is erased; getting into it also stops the
  stream (the player stream and `WOC_TAKEN` take over).
- Others: the entity sink's spawn for `NEK_PARKED_CAR` is
  `ParkedCarManager::NetDriveBound(boundId)` — our own copy
  (`networld::ObjectHandleOf`) switches to `Vehicle::SetNetworkDriven(true)`
  (kinematic `simAICtrl` like traffic, and `mNetworkGhost`, so our hits on it go
  to the owner) and follows the stream; despawn = `NetReleaseBound` → back to
  our physics. If the car's zone is not loaded here, spawn is retried while the
  entity lives.
- `WOC_POSE` is applied by `NetApplyCar` (`Vehicle::SetTransform`) unless the
  car is being moved or driven — also on registration, so a late joiner or a
  reloaded zone places the car where it was left instead of at its locator.
- `RemoveParkedCar` stops our stream and clears network driving first.

### 18.10 Still open
- Pedestrians / actors as entities (`NEK_PEDESTRIAN`/`NEK_ACTOR` reserved).
- No player identity across reconnects (T7).
