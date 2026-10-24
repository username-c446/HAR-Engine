# Multiplayer Sync Manifest

> Living document. Source of truth for which world/game systems must be
> synchronized between players in multiplayer, with file:line references and
> priorities. Produced 2026-10 by a full-codebase audit (parallel exploration
> of world/physics, traffic/NPC/AI, missions/progress, sound/HUD/effects).

## Current sync coverage (baseline)

`PlayerNetworkState` (`src/game/network/PlayerNetworkState.h`) replicates only:
`x/y/z`, `rotY`, `vehicleModel[16]`, `netVehicleId`, `vehicleSpeed`,
`inVehicle`, 2 flags (jump/in-air). Mission sync is a **no-op**
(`NetworkMatchManager::SetMissionStateProvider`, NetworkMatchManager.cpp:127).
Ready transport — `PID_USER` `SendCustomEvent` and the Lua event bus — is not
wired to game content yet.

## Priority-1: visible to everyone, immediately

| System | file:line | What to sync |
|---|---|---|
| PersistentWorldManager bitmask "object destroyed/picked" | data/PersistentWorldManager.cpp:172-199 | Authoritative "no longer exists" list: broken hydrants/crates/trees, picked coins, disabled spawn points |
| VehicleCentral active list + husks | vehiclecentral.h:107,172; .cpp:958,1125; huskpool.cpp:157 | Every car in the world (pose/speed/mVehicleDestroyed), wrecks |
| StatePropDSG `m_CurrentState` | stateprop/stateprop.hpp:88; StatePropDSG.cpp:181,967 | Doors/gates/mechanisms open/closed/broken |
| simAICtrl/simSimulationCtrl + mIsHit | DynaPhysDSG.cpp:182-349 | "asleep/moving/dropped" physical props |
| InstDynaPhysDSG `mMatrix` + Break() | InstDynaPhysDSG.h:110; .cpp:273,488 | Per-frame pose of pushed/thrown props; breaking |
| BreakablesManager::Play | breakablesmanager.cpp:528; 284,491 | Destruction animation + removal (visible to all) |

## Priority-2: NPC reactions, vehicle population

| System | file:line | What to sync |
|---|---|---|
| Ped -> physics on hit (rand impulse) | character/character.cpp:2550-2750 | Instant, visible hit-reaction; host-authoritative, not local rand |
| Ped panic (horn/hit/kick) | ped/pedestrianmanager.cpp:1268-1299 | Crowd reaction around any player |
| Traffic spawn/despawn/husk swap | traffic/trafficmanager.cpp:1489,1553,1250 | Car spawn/despawn/replace + positions |
| Pedestrian spawn/despawn | ped/pedestrianmanager.cpp:649,548 | Models/colors/positions must match |
| Hit-n-Run score/catch + ChaseManager | hitnrunmanager.cpp:500-639; harass/chasemanager.cpp | Shared score; catch disables chases |
| TrafficAI/ChaseAI/WaypointAI states | vehicleai.h:50; chaseai.h:78; waypointai.h | Chase/race/waypoint positions |
| Character TouchProp/AttachProp | character.cpp:4498-4560 | Pickup/throw common prop (collision on/off) |

## Priority-3: mission/progress/timers/states

| System | file:line | What to sync |
|---|---|---|
| MissionSyncState (ready, unwired) | network/MissionSyncState.h:9-27 | missionIndex/stageIndex/objectiveIndex/inMission |
| Mission::mState / mMissionTimer | mission/mission.h:204,214-218 | Mission phase + timer (shared HUD) |
| MissionStage state | mission/missionstage.h:190-199 | Stage progress |
| Script events/cutscenes/triggers | mission/missionscriptloader.*; missionstage.cpp:1590 | Dialogs/CMV/triggers via SendCustomEvent |
| Platform/door animation `mfDirection` | animcollisionentitydsg.h:170; automaticdoor.cpp:21-53 | Auto-doors, cranes, hatches, roboarms |
| Coins: world + bank | coins/coinmanager.h:118-134; .cpp:383,570,651 | World coins collected, shared bank |
| TrafficLight::m_tState | trafficcontrol.h:55,57,76-78 | Light color / roads-to-go (if visual added) |

## Priority-4: audible/visible effects

| System | file:line | What to sync |
|---|---|---|
| World sounds (boom/collision) | sound/soundmanager.cpp:318; movingsoundmanager.cpp:424 | Event+position for positional sound at observer |
| Vehicle explosion/damage smoke | redbrick/vehicle.cpp:6301,5483 | Remote car damage (now a damage-free phantom) |
| Particles/skidmarks/sparkle | render/Particles/*; worldsim/skidmarks/*; coins/sparkle.cpp | Rendered globally but triggered locally |

## Cross-cutting constraints

### World-avatar index ≠ network peer-id (important for readers of this code)

The engine identifies a "player" along two SEPARATE axes — do not conflate them:

- **World-avatar index** — a position in `AvatarManager::mAvatarArray`. The
  avatar count is set by `GameplayManager::GetNumPlayers()`, and the mission
  system fixes it at **1** (`missionmanager.cpp:174`, `SetNumPlayers(1)`). So
  `GetAvatarForPlayer(0)` is the ONE and ONLY avatar of this process — the
  LOCAL player of whichever machine this is. It is NOT "the host" in any
  network sense. (Exception: local split-screen `supersprintmanager.cpp:2614`
  sets `GetNumPlayers()>1`; there index 0 = first local P1.)
- **Network peer-id** — `net::Session::GetLocalPeerId()` (field `m_selfId`),
  assigned by the server in connect order: `ServerEndpoint` assigns
  `id = m_nextPeerId++` (ServerEndpoint.cpp:67). The host process connects to
  itself first (loopback in `StartHost`), so it gets **id 0**; joining clients get
  **1, 2, 3…**. This id exists only for routing and self-echo filtering — it is
  NOT an index into `AvatarManager`.

**Why this matters:** the coincidence "world-avatar 0 == network id 0" on the
host is an artifact of connect order, not a semantic link. On a client the
network self-id is 1 while the world avatar is still index 0. Do not pass a
network id into `GetAvatarForPlayer(id)`, and do not read
`GetAvatarForPlayer(0)` as "first of many": with `SetNumPlayers(1)` it just
means "the local one".

**What this guarded against (the bug):** `GamePlayerBridge` kept `m_selfId = 0`
by default and filtered "echo" via `from == m_selfId`
(GamePlayerBridge.cpp:43). On a client this dropped the HOST's state (whose
peer id is exactly 0), so the host never showed up as a remote. Fixed:
`NetworkMatchManager::Update()` syncs the bridge with the session's real
self-id every frame (`m_bridge->SetLocalPeerId(GetLocalClientId())`). Do not
remove that sync — bridge echo filtering would become wrong again.

- **Almost all NPC/traffic systems hard-code `GetAvatarForPlayer(0)`** (~140
  sites): peds, traffic, AI actors react only to player 0. Needed: iterate all
  avatars (framework added — `AvatarManager::GetNumAvatars()` and
  `FindNearestAvatar()`; panic-ped already loops over all avatars) or
  host-authoritative result broadcast.
- **Heavy `rand()` dependence** (car color, hit impulse, chosen ped-driver):
  pure deterministic replay is unreliable → prefer host-authoritative
  calculation + event broadcast via `net::`.
- **World clock/global time absent**: static world seeded by `worldSeed`
  (`Game::GetRandomSeed()`); dynamics stay local by design
  (docs/WORLD_SYNC_PLAN.ru.md §3-5). Variable `dt` (game.cpp:483) rules out full
  lockstep.

## Recommended order

1. PersistentWorldManager bitmask (destroyed-object dictionary).
2. VehicleCentral active list + husks + `mVehicleDestroyed`.
3. StatePropDSG `m_CurrentState` + platform/door `mfDirection`.
4. "Awakened" dynamics with `mMatrix`.
5. Destruction/explosion events (relay via `PID_USER`).