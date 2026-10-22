# MultiTool — «The Simpsons: Hit & Run» (SRR2) — Multiplayer Specification

> **Status:** living document. Update and consult it during every development
> cycle, so you never create duplicate functionality. Mark implemented features
> and describe new function relationships as they appear.

This file describes all programmatically added gameplay on top of the original
SRR2 and, above all, the multiplayer stack — how the pieces interact. It is
intended both for another AI/developer studying the project and for the
assistant itself, so it doesn't get lost.

---

## 1. Project map (network area)

All networking files live in `src/game/network/`. Main projections:

| File | Role |
|------|------|
| `NetworkPacket.h` | Packet format, types, little-endian serialization, constants |
| `PlayerNetworkState.{h,cpp}` | Player state struct (position/rotation/vehicle) + serialization |
| `NetworkServer.{h,cpp}` | Authoritative server: admission, sessions, validation, relay |
| `NetworkClient.{h,cpp}` | Client: connection, heartbeat, receiving registry/states |
| `NetworkMatchManager.{h,cpp}` | Orchestrator: host/join/leave, local-state provider |
| `RemotePlayerManager.{h,cpp}` | Remote-player registry (keyed by clientId) |
| `RemotePlayerWorldView.{h,cpp}` | Visualization of remote on-foot players + interpolation |
| `RemoteVehicleWorldView.{h,cpp}` | Visualization of "ghost" vehicles of remote drivers |
| `ServerModManager.{h,cpp}` | Server-side Lua mods (admission/validation hooks) |
| `MissionSyncManager.{h,cpp}` | Mission/stage/objective sync between clients (co-op progress) |
| `MissionSyncState.{h,cpp}` | Serializable mission state (mission/stage/objective) |
| `NetworkConsoleCommands.{h,cpp}` | Console commands `host`/`join`/`match` |
| `ENetInit.h` | RAII reference-counting for ENet |

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
    uint8 padding;            // size alignment
};
```

- `SERIALIZED_SIZE = 4*4 + 16 + 4 + 4 + 1 + 1 = 42` bytes.
- Little-endian-compatible serialization, free of padding bugs
  (`Serialize`/`Deserialize` in `PlayerNetworkState.cpp` via `net::Write*`).
- **Validation link:** `inVehicle` picks the speed limit on the server
  (`kMaxSpeedOnFoot ≈ 15 m/s`, `kMaxSpeedInVehicle ≈ 120 m/s`).
- **Vehicle link:** `vehicleModel` + `netVehicleId` go to `RemoteVehicleWorldView`
  so the receiver spawns a ghost vehicle of the same model. `netVehicleId` is
  derived from `mVehicleCentralIndex` (or a pointer hash as fallback) — stable
  within a session.

---

## 4. Server `NetworkServer`

### Connection (admission)
1. ENet connect → assign `clientId` (`m_nextClientId++`, starts at 1000).
2. **Lua mod** may reject: `OnPlayerJoin(clientId)` == false → disconnect.
3. Create `ClientSession` (peer, timestamps, `authenticated`).
4. If password is enabled (`m_requiresAuth`) — wait for `AUTH`; without password —
   `WelcomeClient()` immediately.

### Sessions (`ClientSession`)
- `peer`, `lastStateTimeMs`, `connectedAtMs`, `lastSeenMs`, `validated`,
  `authenticated`.
- `lastSeenMs` updated on **any** incoming packet → timeout sweep.

### Heartbeat / dead sweep
- `PACKET_PING` → server replies `PONG` and updates `lastSeenMs`.
- At the end of `Update()` a sweep runs: if `now - lastSeenMs > m_clientTimeoutMs`
  → the client is considered dead, `OnPlayerLeave` is called, it is removed from
  all structures and `enet_peer_disconnect`.
- `SetClientTimeoutMs` overrides the window (for tests).

### Validation and relay (`ValidateAndRelay`)
- Plausibility check: teleports/overspeeds are rejected; speed depends on
  `inVehicle`. On exceed, the state is not relayed (last honest state stays in
  the registry).
- Lua hook `OnPlayerValidate` may tighten further.
- Accepted state goes into `m_players` and is relayed to all other clients as
  `PLAYER_STATE` (with sender = `clientId`).

### Entity registry (`SendPlayerListToAll`)
- After each connect/disconnect/sweep the server sends `PLAYER_LIST` to all:
  `uint16 count` + array of `clientId`. This is the authoritative source of world
  composition.

### Authentication (`PACKET_AUTH`)
- Server stores `m_password[65]` + `m_requiresAuth`.
- If a password is set and the client sends the correct one — marked
  `authenticated` and welcomed; otherwise `enet_peer_disconnect`.

### Configuration
- `SetMaxPlayers(slots)` — limit on concurrent connections (only before `Start`).
- `SetPassword(pw)` — enable protection; `SetClientTimeoutMs`.

---

## 5. Client `NetworkClient`

- `Connect(ip, port)` → ENet host, `SetPassword` if any.
- After ENet connect sends `HELLO` and, if a password was set, `AUTH`.
- On `WELCOME` stores `m_clientId` and transitions to `m_serverReady = true`,
  resetting `remotePlayers`/authoritative list.
- `PLAYER_LIST` → rebuild remote set, **diff** against the previous authoritative
  list generates join/leave events (see §7).
- `PLAYER_STATE` from other clients → `UpdatePlayer` into `m_remotePlayers`
  (own echo ignored by `clientId`).
- Heartbeat: every `m_pingIntervalMs` sends `PING`, stores `m_pingSentMs`;
  on `PONG` computes RTT → `GetPingMs()` / `HasMeasuredPing()`.
- `SendPlayerState` — rate-limited (`kStateEveryFrames = 6`, ~10 Hz).

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

### Sink in gameplay context (`RemoteVehicleSpawn/Move/Despawn`)
- Spawn via `VehicleCentral::InitVehicle(model, addToActiveList, ...,
  VT_TRAFFIC, FORCE_NO_DRIVER, ...)` — sans driver, vehicle created as a normal
  active one.
- Move — `SetPosition`; Despawn — `RemoveVehicleFromActiveList` + `delete`.

---

## 7. Players: list and notifications (C8/C9)

- `NetworkClient` on `PLAYER_LIST` does a diff between `m_prevAuthoritativePlayers`
  and the new list:
  - id existed, now absent → leave event;
  - id absent, now present → join event.
- Events accumulate in `m_pendingEvents` (`JoinLeaveEvent{clientId, joined}`)
  and are drained via `DrainJoinLeaveEvents` (through `NetworkMatchManager`).
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

- `--password` → `server.SetPassword()` / `client.SetPassword()`.
- `--max` → `server.SetMaxPlayers()`.
- `--log` → log file (launch line + `clients:N` on roster change).
- `--mod` → `server.LoadServerMod()`.

### Main game CLI (`CommandLineOptions`)
- `HOSTPORT=<port>` — auto-start host.
- `JOINIP=<ip[:port]>` — auto-start join.
- `HOSTPASS=<pw>`, `HOSTSLOTS=<n>`, `JOINPASS=<pw>` — configuration.
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

Single network entry point from gameplay. One process = **either host or
join-client, never both** (`HostAndJoinAreMutuallyExclusive`).

- `StartHost(port)` / `JoinHost(ip, port)` / `Leave()` — match lifecycle.
- `SetHostConfig(password, maxPlayers)`, `SetClientPassword(pw)` — config.
- `SetLocalStateProvider(fn)` — local-state provider.
- `GetRemotePlayers()` — registry (from server or client).
- `DrainJoinLeaveEvents(out)` — join/leave notifications.
- `Update()` — spins server/client; on the client sends the local state via the
  provider on a timer.

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
- Run: `vstest.console.exe` against `Tests.dll`. Current state — 30/30 green.
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
- **MP stability** — verify end-to-end host/join entry in-game (character is
  controlled, interaction key works, clean exit without asserts), a networked
  match between two copies.

**Recorded (not a network bug):** rare one-off flake "world load sticks almost
at the end" entering as host through the menu. Investigation: the network
`NetworkMatchManager::Update` is non-blocking (`enet_host_service` with timeout 0),
called only after the world loads (gameplaycontext.cpp:785), unrelated to the
hang. `skipfe HOSTPORT=3737` (same network start) and the plain start passed
clean over 7+ runs (resp, no crash); `0xc0000005` crashes in the system log were
from old builds. Conclusion: this is an intermittent level-load flake (fixed by
restarting), not a multiplayer issue.

---

## 16. Rule for the future

**Before and during any development, consult this document.** If a feature is
already described, do not recreate it and do not add a duplicate transport/packet
(example: `PACKET_VEHICLE_STATE` is declared, but vehicle data lives in
`PLAYER_STATE` — don't make a second path). Update this file after major changes.