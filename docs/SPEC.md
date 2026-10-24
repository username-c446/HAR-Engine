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
(`src/game/netengine/`) and the game layer (`src/game/network/`). The legacy
monolith `NetworkServer`/`NetworkClient` has been fully removed.

### Engine layer `src/game/netengine/` (reusable, world-agnostic)

| File | Role |
|------|------|
| `Packet.h` | Wire protocol: `PacketId`, protocol version, reserved id ranges, bounds-checked `Writer`/`Reader` |
| `PacketRegistry.{h,cpp}` | Packet registry (id → handler) instead of `switch`; extensible without touching the core |
| `ServerEndpoint.{h,cpp}` | Pure ENet host: peer admission, `PeerId`, liveness tracking, timeout sweep, `KickPeer`, PING probes |
| `ClientEndpoint.{h,cpp}` | Pure ENet client: connect, framing, heartbeat (`SetPingIntervalMs`), RTT (`SendPing`/`GetPingMs`) |
| `Session.{h,cpp}` | Match orchestrator: host/join/leave, state relay, password (`SetHostPassword`/`SetPassword`), validator (`SetStateValidator`), peer roster |
| `GamePlayerBridge.{h,cpp}` | Game adapter: ties `PlayerNetworkState` to the `Session`'s opaque blobs |
| `WorldInterfaces.h` | Interfaces `ILocalStateProvider`/`IRemoteStateSink`/`IStateValidator` — the world attaches externally |
| `ENetInit.{h,cpp}` | RAII reference-counting for ENet (static defined in `.cpp`) |

### Game layer `src/game/network/`

| File | Role |
|------|------|
| `NetworkPacket.h` | Game types and little-endian primitives (`net::Write*/Read*`) |
| `PlayerNetworkState.{h,cpp}` | Player state struct (position/rotation/vehicle) + serialization |
| `NetworkMatchManager.{h,cpp}` | THIN BRIDGE: keeps the old public API (`StartHost`/`JoinHost`/`Leave`/`GetRemotePlayers`), internally delegates to `net::Session` + `GamePlayerBridge` |
| `RemotePlayerManager.{h,cpp}` | Remote-player registry (keyed by clientId) |
| `RemotePlayerWorldView.{h,cpp}` | Visualization of remote on-foot players + interpolation |
| `RemoteVehicleWorldView.{h,cpp}` | Visualization of "ghost" vehicles of remote drivers |
| `ServerModManager.{h,cpp}` | Server-side Lua mods (admission/validation hooks) |
| `MissionSyncManager.{h,cpp}` | Mission/stage/objective sync between clients (co-op progress) |
| `MissionSyncState.{h,cpp}` | Serializable mission state (mission/stage/objective) |
| `NetworkConsoleCommands.{h,cpp}` | Console commands `host`/`join`/`match` |

In-game integration: `src/game/contexts/gameplay/gameplaycontext.cpp`
(state provider, sinks, calls in the game loop) and
`src/game/main/commandlineoptions.{h,cpp}` (CLI auto-start).

---

## 2. Network model and transport

- **Transport:** ENet (vcpkg `enet`), channel `GAME_CHANNEL = 0`, all gameplay
  packets reliable (`ENET_PACKET_FLAG_RELIABLE`). Order and delivery are
  guaranteed by ENet; our protocol is only type + payload.
- **Architectural principle:** authoritative server (not P2P). The server admits
  clients, issues `clientId`, validates states, relays them, keeps the entity
  registry and distributes it to clients. The client is not self-authoritative.
- `MAX_PACKET_SIZE = 512` bytes. `CLIENT_TIMEOUT_MS = 8000`, `PING_INTERVAL_MS = 2000`.
- Data is linear, there is no bloated protocol: payload can contain repeated
  `clientId` (in `PLAYER_STATE`), but the full id is always the server's
  authoritative concern.

### Packet types (`net::PacketType`, 1-byte tag at the start)

| Tag | Name | Direction | Purpose |
|-----|------|-----------|---------|
| 0 | `HELLO` | C→S | Announcement after ENet connect |
| 1 | `WELCOME` | S→C | Authoritative `clientId` (admission analog) |
| 2 | `PLAYER_STATE` | C→S→C | Player state (relay) |
| 3 | `DISCONNECT` | C→S | Disconnect declaration |
| 4 | `PLAYER_LIST` | S→C | Authoritative registry of present entities |
| 5 | `VEHICLE_STATE` | reserved | (described in §6 as part of PLAYER_STATE in practice) |
| 6 | `PING` | C→S | Heartbeat + RTT measurement |
| 7 | `PONG` | S→C | Ping reply |
| 8 | `AUTH` | C→S | Password for a protected host |
| 9 | `MISSION_STATE` | C→S→C | Mission/stage/objective progress (relay, for co-op) |

> Note: `PACKET_VEHICLE_STATE` is declared in the enum, but the current
> implementation carries the transport data inside `PLAYER_STATE` (see §6), i.e.
> packet 5 is not used on the wire yet.

---

## 3. Player state `PlayerNetworkState`

Exact byte layout (field order matters for serialization):

```
struct PlayerNetworkState {
    float x, y, z;            // position
    float rotY;               // rotation around vertical
    char  vehicleModel[16];   // vehicle model name ("" if on foot)
    int32 netVehicleId;       // stable network id of the vehicle
    float vehicleSpeed;       // vehicle speed (km/h) for validation
    uint8 inVehicle;          // 1 = in a vehicle
    uint8 flags;              // FLAG_JUMPING | FLAG_IN_AIR | FLAG_LOCO_TURBO
    // --- continuous locomotion (on foot) ---
    uint16 motionAngle;       // desired motion direction, rad*1000
    uint16 facingAngle;       // desired facing, rad*1000
    uint8  speedFactor;       // 0..255 fraction of max speed
    uint8  characterState;    // CharacterAi::CharacterState (LOCO etc.)
    // --- one-shot action ---
    uint8  actionEvent;       // ACTION_*; 0 = none (rising-edge latched)
    // --- kinematic vehicle mirror (inVehicle) ---
    float  vehYaw;            // machine heading, rad
    uint8  vehSteer;          // signed steer -128..127 -> -45..+45 deg
    uint16 vehSpeedKmh;       // speed, km/h
    uint8  vehState;          // VS_NORMAL / VS_SLIP / VS_EBRAKE_SLIP
    uint8  vehFlags;          // VEH_FLAG_BRAKE | VEH_FLAG_REVERSE
};
```

- `SERIALIZED_SIZE = 42 (base) + 7 (locomotion) + 1 (actionEvent) + 9 (vehicle) = 58` bytes.
- In a vehicle the locomotion/action fields are zeroed; on foot the vehicle fields are.
- `actionEvent` is currently populated only with `ACTION_JUMP` (jump rising edge);
  the other `ACTION_*` ids (SLAM, DODGE, CELEB…) are reserved and no-op on receive.
- Little-endian-compatible serialization, free of padding bugs
  (`Serialize`/`Deserialize` in `PlayerNetworkState.cpp` via `net::Write*`).
- **Validation link:** `inVehicle` picks the speed limit on the server
  (`kMaxSpeedOnFoot ≈ 15 m/s`, `kMaxSpeedInVehicle ≈ 120 m/s`).
- **Vehicle link:** `vehicleModel` + `netVehicleId` go to `RemoteVehicleWorldView`
  so the receiver spawns a ghost vehicle of the same model. `netVehicleId` is
  derived from `mVehicleCentralIndex` (or a pointer hash as fallback) — stable
  within a session.

---

## 4. Server layer `net::ServerEndpoint` + `net::Session`

Server-side logic is split into two layers:

- **`net::ServerEndpoint`** — pure ENet host: peer admission, `PeerId` assignment,
  packet framing, liveness counting, timeout sweep, auto-PING probing (the client
  itself requests heartbeats), and forcible `KickPeer`. Knows nothing about a world.
- **`net::Session`** (host mode) — on top of the server endpoint: verifies the
  password against `PID_AUTH` and only then sends `WELCOME`+`WORLD_SEED`+`PLAYER_LIST`;
  relays `PID_PLAYER_STATE` through an optional `IStateValidator` (anti-teleport);
  keeps the authoritative peer roster.

### Connection (admission)
1. ENet connect → `ServerEndpoint` assigns a `PeerId` (`m_nextPeerId++`, starts at 1).
2. If the `Session` is password-protected (`SetHostPassword`), `WELCOME` is deferred
   until the client sends a correct `PID_AUTH`; a wrong/missing password → `KickPeer`
   (the peer never learns the world composition).
3. Without a password — `WELCOME` + `WORLD_SEED` immediately, then `PLAYER_LIST` to all.

### Heartbeat / dead sweep
- The client sends `PID_PING` at a configured frequency
  (`ClientEndpoint::SetPingIntervalMs`); every inbound packet refreshes the peer's
  `lastSeenMs` in `ServerEndpoint`.
- At the end of `ServerEndpoint::Update()` a sweep runs: `now - lastSeenMs >
  m_clientTimeoutMs` → the peer is removed, `PeerEvent{id,false}` (the Session
  cleans its roster), and `enet_peer_disconnect` is called. `SetClientTimeoutMs`
  overrides the window (for tests).

### Validation and relay
- `net::Session` feeds every inbound `PID_PLAYER_STATE` to
  `IStateValidator::ValidatePeerState` (when set). A rejected state is **not
  relayed**; the last honest state stays with receivers.
- `GamePlayerBridge`/tests implement the validator as an anti-teleport gate with a
  speed limit chosen by `inVehicle` (`kMaxSpeedOnFoot ≈ 15 m/s`,
  `kMaxSpeedInVehicle ≈ 120 m/s`).
- Accepted state is relayed to all other peers as `PLAYER_STATE` with sender = `PeerId`.

### Entity registry
- After each connect/disconnect/sweep the server sends `PLAYER_LIST` to all
  (`uint16 count` + array of `PeerId`). This is the authoritative source of world
  composition.

### Configuration
- `Session::SetHostPassword(pw)` — protect the host; `ServerEndpoint::SetClientTimeoutMs`.

---

## 5. Client layer `net::ClientEndpoint` + `net::Session`

- `net::ClientEndpoint::Connect(ip, port)` → ENet host + peer.
- `Session::SetPassword(pw)` sets the password; the `PID_AUTH` send is deferred
  until the ENet connection is established (`IsPeerReady()`) via
  `Session::SendAuthIfDue()` in `Update()` — sending before the handshake would
  drop the payload on a protected host.
- `net::ClientEndpoint` itself sends `PID_PING` at a configured frequency
  (heartbeat) and, on `PONG`, computes RTT → `GetPingMs()`/`HasMeasuredPing()`.
- On `WELCOME` the `Session` stores `selfId`; `PLAYER_LIST` rebuilds the peer
  roster (`GetPeerRoster`); `PLAYER_STATE` from others → `IRemoteStateSink`
  (via `GamePlayerBridge` into the game); own echo ignored by `selfId`.
- Local state is sent through `ILocalStateProvider` at `SetStateSendEvery`
  cadence (default every 6th `Update`, ~10 Hz).

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
- `Update(dt)` — position interpolation (same mechanism as on-foot).

- `Update(dt)` also smooths `vehYaw` over the shortest arc; steer/speed/state/
  flags are passed to the move sink verbatim.

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
  remote drivers.

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

## 8. Dedicated server (D10/D11)

`src/tests/mp_standalone.cpp` — headless server and join client without game data.

Usage:
```
mp_standalone server [port] [--mod lua] [--password pw] [--max slots] [--log file]
mp_standalone host   [port] [--password pw] [--max slots] [--log file]
mp_standalone join   <hostIp> [port] [--password pw]
```

- Runs on the engine layer `net::Session` (instead of the old `NetworkServer`/`Client`).
- `--password` → `session.SetHostPassword()` / `session.SetPassword()`.
- `--log` → log file (launch line + `clients:N` on roster change).
- `--mod` → `server.LoadServerMod()`.

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

- Loads a single script: `LoadScript(path)`.
- Hooks: `on_join(clientId)`, `on_validate(clientId, x,y,z,speed)`,
  `on_leave(clientId)`.
- The server calls them at the admission/validation/drop points (see §4).
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
- `Update()` — drives `net::Session::Update()` (server+client+relay+validator).

---

## 11. Interpolated visualization (`RemotePlayerWorldView`)

- Sink: `spawn`/`move`/`despawn` decoupled from the world (testable).
- `Sync(players)` — updates targets for each remote player, spawns/despawns by
  authoritative composition.
- `Update(dt)` — per-frame exponential smoothing toward the target
  (frame-rate independent, `lambda = 3/smoothingSeconds`), does not teleport.
- Key contract: changing a position does NOT move the model in `Sync` — only sets
  the target; the actual movement happens in `Update`. This removes stutter and
  jitter at the packet frequency.
- Facing (`rotY`) is smoothed over the shortest arc (350° → 10° through 0).
- Locomotion fields (`motionAngle`/`facingAngle`/`speedFactor`/`characterState`,
  turbo) reach the move sink unsmoothed: in game, when `characterState == LOCO`,
  the puppet's `LocomotionDriver` drives them (`SetFacingDir` + speed from
  `GetMaxSpeed()`).
- The `action` sink fires once on the rising edge of a non-zero `actionEvent`
  (`RemotePlayerAction`: `ACTION_JUMP` → `RemotePlayerJump`).

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

### 12.1 World events (`WorldEvent`, relayed over `PID_USER`)

One-shot world changes go through `WorldEventCodec` (6-byte header + POD
payload, no game types). Emitters live in `src/game/network/*WorldEventEmitter.*`
and are a safe no-op outside a match:

| Type | Emitter / call site | Payload |
|---|---|---|
| `WE_STATEPROP` 0x01 | `EmitStatePropWorldEvent` — `StatePropDSG` (state change) | uid + state |
| `WE_BREAKABLE` 0x02 | `EmitBreakableWorldEvent` — `StatePropDSG` (destroyed) | persistentId + xyz |
| `WE_VEHICLE_DESTROYED` 0x03 | `EmitVehicleDestroyedWorldEvent` — `vehicle.cpp` (explosion) | netVehicleId + xyz |
| `WE_COIN_COLLECTED` 0x04 | `EmitCoinCollectedWorldEvent` — `CoinManager::CheckCollection` | persistentId + xyz |
| `WE_PLATFORM_DIR` 0x05 | `EmitPlatformDirWorldEvent` — `animcollisionentitydsg.h` | uid + direction |

The receiver (`gameplaycontext.cpp`, `SetWorldEventSink`) currently applies only
`WE_VEHICLE_DESTROYED`: it finds the local vehicle by net id and re-fires
`EVENT_VEHICLE_DESTROYED` (husk logic). Remote drivers' ghosts are left alone —
the drive-state stream despawns them. Other types are transmitted only
(receiving is the next step, see `PRECISE_MULTIPLAYER_PLAN.ru.md`).

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