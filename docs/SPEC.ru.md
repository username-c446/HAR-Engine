# HAR-Engine — Спецификация сетевого слоя

> **Статус:** живой документ. Обновляй и сверяйся с ним при каждой разработке,
> чтобы не создавать дублирующий функционал. Помечай реализованное фичами и
> описывай новые связи функций по мере их появления.

> Проект развивается как **самостоятельный игровой движок**; исходной базой кода
> послужила игра _The Simpsons: Hit & Run_ (современная сообществная пересборка).

> **Миграция завершена (2026-10).** Старый монолит `NetworkServer`/`NetworkClient`
> полностью удалён из кода и проектов. Весь транспорт, авторизация, валидация и
> релей вынесены в переиспользуемый движковый слой `src/game/netengine/`
> (`net::Session`, `net::ServerEndpoint`/`ClientEndpoint`, `net::Packet`,
> `net::PacketRegistry`, `world-интерфейсы`). `NetworkMatchManager` теперь —
> тонкий мост поверх `net::Session` + `GamePlayerBridge`, сохраняющий старый
> игровой АПИ. Защитные фичи монолита (таймаут-кик, heartbeat/ping/RTT, пароль,
> анти-телепорт) перенесены в netengine; тесты 42/42 зелёные.

Этот файл описывает сетевой слой движка и то, как его части взаимодействуют
между собой. Он предназначен и для другого разработчика/ИИ, изучающего проект,
и для самого ассистента, чтобы не путаться. World-терминология («игрок»,
«машина», «миссия») относится к контенту, который движок загружает через
интерфейсы мира, а не к привязке к какой-либо конкретной игре.

---

## 1. Карта проекта (область сети)

Сетевой слой движка разбит на **независимые слои**: игровой слой
(`src/game/network/`) и переиспользуемый движковый слой (`src/game/netengine/`).
Старый монолит `NetworkServer`/`NetworkClient` полностью выпилен.

### Движковый слой `src/game/netengine/` (переиспользуемый, world-agnostic)

| Файл | Роль |
|------|------|
| `Packet.h` | Он-найр протокол: `PacketId`, версия, зарезервированные id (0x00-0xFF системные, 0x100+ игровые), bounds-checked `Writer`/`Reader` |
| `PacketRegistry.{h,cpp}` | Реестр пакетов (id → обработчик) вместо `switch`; расширяем без правки ядра |
| `ServerEndpoint.{h,cpp}` | Чистый ENet-хост: допуск пиров, `PeerId`, счёт liveness, таймаут-свип, `KickPeer`, авто-PING-проб |
| `ClientEndpoint.{h,cpp}` | Чистый ENet-клиент: коннект, фрейминг, heartbeat (`SetPingIntervalMs`), замер RTT (`SendPing`/`GetPingMs`) |
| `Session.{h,cpp}` | Оркестратор матча: host/join/leave, релей состояния, пароль (`SetHostPassword`/`SetPassword`), валидатор (`SetStateValidator`), реестр пиров |
| `GamePlayerBridge.{h,cpp}` | Игровой адаптер: связывает `PlayerNetworkState` с opaque-блобами `Session`'а |
| `WorldInterfaces.h` | Интерфейсы `ILocalStateProvider`/`IRemoteStateSink`/`IStateValidator` — мир подключается снаружи |
| `ENetInit.{h,cpp}` | RAII-счётчик ссылок ENet (статика определена в `.cpp`) |

### Игровой слой `src/game/network/`

| Файл | Роль |
|------|------|
| `NetworkPacket.h` | Игровые типы и little-endian-примитивы (`net::Write*/Read*`) |
| `PlayerNetworkState.{h,cpp}` | Структура состояния игрока (позиция/поворот/машина) + сериализация |
| `NetworkMatchManager.{h,cpp}` | ТОНКИЙ МОСТ: сохраняет старый публичный АПИ (`StartHost`/`JoinHost`/`Leave`/`GetRemotePlayers`), внутри делегирует `net::Session` + `GamePlayerBridge` |
| `RemotePlayerManager.{h,cpp}` | Реестр удалённых игроков (идент. по clientId) |
| `RemotePlayerWorldView.{h,cpp}` | Визуализация удалённых пеших игроков + интерполяция |
| `RemoteVehicleWorldView.{h,cpp}` | Визуализация «призраков» машин удалённых водителей |
| `ServerModManager.{h,cpp}` | Серверные Lua-моды (хуки допуска/валидации) |
| `MissionSyncManager.{h,cpp}` | Синхронизация миссии/стадии/цели между клиентами (co-op-прогресс) |
| `MissionSyncState.{h,cpp}` | Сериализуемое состояние миссии (миссия/стадия/цель) |
| `NetworkConsoleCommands.{h,cpp}` | Консольные команды `host`/`join`/`match` |

Интеграция в игру: `src/game/contexts/gameplay/gameplaycontext.cpp`
(провайдер состояния, sink-и, вызовы в игровом цикле) и
`src/game/main/commandlineoptions.{h,cpp}` (автостарт из CLI).

---

## 2. Сетевая модель и транспорт

- **Транспорт:** ENet (vcpkg `enet`), единственный канал, все игровые пакеты
  надёжные (`ENET_PACKET_FLAG_RELIABLE`). Порядок и доставку гарантирует ENet;
  свой протокол — только id + payload.
- **Архитектурный принцип:** авторитетный сервер (а не P2P). Сервер допускает
  клиентов, выдаёт `PeerId`, валидирует состояния, ретранслирует их, ведёт
  реестр пиров и раздаёт его клиентам. Клиент не само-авторизуется.
- `MAX_PACKET_PAYLOAD = 512` байт. Таймаут liveness по умолчанию 30 с
  (переопределяется `SetClientTimeoutMs`), heartbeat-кадр по умолчанию 60 Update.
- Данные линейны: payload может содержать повторяющиеся `PeerId` (в `PLAYER_STATE`),
  но полный id — всегда авторитетная забота сервера.

### Типы пакетов (`net::ReservedPacketId`, 2-байтовый id в начале)

| Id | Имя | Направление | Назначение |
|----|-----|-------------|------------|
| 0x0000 | `HELLO` | C→S | Анонс после ENet-коннекта |
| 0x0001 | `WELCOME` | S→C | Авторитетный `PeerId` (self id) |
| 0x0002 | `ACK` | S/C | Квитирование версии/liveness |
| 0x0003 | `AUTH` | C→S | Пароль для защищённого хоста |
| 0x0004 | `PLAYER_LIST` | S→C | Авторитетный реестр присутствующих пиров |
| 0x0005 | `PING` | C→S | Heartbeat + замер RTT |
| 0x0006 | `PONG` | S→C | Ответ на ping |
| 0x0007 | `WORLD_SEED` | S→C | Авторитетный сид мира (u32) |
| 0x0100 | `PLAYER_STATE` | C→S→C | Состояние игрока (релей) |
| 0x0101 | `VEHICLE_STATE` | зарезервирован | (данные машин переносятся внутри PLAYER_STATE) |
| 0x0102 | `MISSION_STATE` | C→S→C | Прогресс миссии/стадии/цели (релей, для co-op) |

> Примечание: `PACKET_VEHICLE_STATE` объявлен в enum, но текущая реализация
> переносит транспортные данные внутри `PLAYER_STATE` (см. §6), т.е. пакет 0x0101
> пока не используется на проводе.

---

## 3. Состояние игрока `PlayerNetworkState`

Точное байтовое представление (порядок полей важен для сериализации):

```
struct PlayerNetworkState {
    float x, y, z;            // позиция
    float rotY;               // поворот вокруг вертикали
    char  vehicleModel[16];   // имя модели машины ("" если пешком)
    int32 netVehicleId;       // стабильный сетевой id машины
    float vehicleSpeed;       // скорость машины (км/ч) для валидации
    uint8 inVehicle;          // 1 = в машине
    uint8 flags;              // FLAG_JUMPING | FLAG_IN_AIR | FLAG_LOCO_TURBO
    // --- непрерывный локомоушн (пешком) ---
    uint16 motionAngle;       // желаемое направление движения, рад*1000
    uint16 facingAngle;       // желаемый фейсинг, рад*1000
    uint8  speedFactor;       // 0..255 доля максимальной скорости
    uint8  characterState;    // CharacterAi::CharacterState (LOCO и т.п.)
    // --- одноразовое действие ---
    uint8  actionEvent;       // ACTION_*; 0 = нет (латч по rising-edge)
    // --- кинематическое зеркало машины (inVehicle) ---
    float  vehYaw;            // курс машины, рад
    uint8  vehSteer;          // знаковый руль -128..127 -> -45..+45°
    uint16 vehSpeedKmh;       // скорость, км/ч
    uint8  vehState;          // VS_NORMAL / VS_SLIP / VS_EBRAKE_SLIP
    uint8  vehFlags;          // VEH_FLAG_BRAKE | VEH_FLAG_REVERSE
};
```

- `SERIALIZED_SIZE = 42 (база) + 7 (локомоушн) + 1 (actionEvent) + 9 (машина) = 58` байт.
- В машине поля локомоушна/действия обнуляются, пешком — поля машины.
- `actionEvent` сейчас заполняется только `ACTION_JUMP` (rising-edge прыжка);
  остальные `ACTION_*` (SLAM, DODGE, CELEB…) зарезервированы, на приёмнике no-op.
- Сериализация little-endian совместимая, свободна от padding-багов
  (`Serialize`/`Deserialize` в `PlayerNetworkState.cpp` через `net::Write*`).
- **Связь с валидацией:** поле `inVehicle` выбирает лимит скорости на сервере
  (`kMaxSpeedOnFoot ≈ 15 м/с`, `kMaxSpeedInVehicle ≈ 120 м/с`).
- **Связь с машинами:** `vehicleModel` + `netVehicleId` идут в
  `RemoteVehicleWorldView`, чтобы приёмник создал «призрака» машины той же
  модели. `netVehicleId` выводится из `mVehicleCentralIndex` (или хэша указателя
  как запас) — стабилен в рамках сессии.

---

## 4. Серверный слой `net::ServerEndpoint` + `net::Session`

Серверная сторона разнесена на два слоя (см. §1):

- **`net::ServerEndpoint`** — чистый ENet-хост: допуск пиров, назначение `PeerId`,
  фрейминг пакетов, счёт liveness, таймаут-свип, авто-PING-проб (heartbeat
  запрашивает клиент сам), принудительный `KickPeer`. Ничего не знает про мир.
- **`net::Session`** (host-режим) — поверх серверного эндпоинта: при приёме
  `PID_AUTH` сверяет пароль и только тогда шлёт `WELCOME`+`WORLD_SEED`+`PLAYER_LIST`;
  релее `PID_PLAYER_STATE` через **опциональный `IStateValidator`** (анти-телепорт);
  ведёт авторитетный реестр пиров.

### Допуск
1. ENet-коннект → `ServerEndpoint` назначает `PeerId` (`m_nextPeerId++`, стартует с 1).
2. Если `Session` защищён паролем (`SetHostPassword`), `WELCOME` откладывается,
   пока клиент не пришлёт корректный `PID_AUTH`; неправильный/отсутствующий пароль
   → `KickPeer` (пир никогда не узнаёт состав мира).
3. Без пароля — `WELCOME` + `WORLD_SEED` сразу, затем `PLAYER_LIST` всему составу.

### Heartbeat / таймаут
- Клиент сам шлёт `PID_PING` с заданной частотой (`ClientEndpoint::SetPingIntervalMs`);
  каждый входящий пакет обновляет `lastSeenMs` пира в `ServerEndpoint`.
- В конце `ServerEndpoint::Update()` — свип: `now - lastSeenMs > m_clientTimeoutMs`
  → пир удаляется, `PeerEvent{id,false}` (Session чистит реестр/рекстур), и вызывается
  `enet_peer_disconnect`. `SetClientTimeoutMs` переопределяет окно (для тестов).

### Валидация и релей
- `net::Session` на каждый входящий `PID_PLAYER_STATE` вызывает
  `IStateValidator::ValidatePeerState` (если задан). Отклонённое состояние
  **не ретранслируется**; последнее честное остаётся у получателя.
- `GamePlayerBridge`/тесты реализуют валидатор как анти-телепорт с лимитом
  скорости в зависимости от `inVehicle` (`kMaxSpeedOnFoot ≈ 15 м/с`,
  `kMaxSpeedInVehicle ≈ 120 м/с`).
- Принятое состояние ретранслируется всем остальным пирам как `PLAYER_STATE`
  с полем sender = `PeerId`.

### Реестр сущностей
- После каждого подключения/отключения/свипа сервер шлёт всем `PLAYER_LIST`
  (`uint16 count` + массив `PeerId`). Это авторитетный источник состава мира.

### Конфигурация
- `Session::SetHostPassword(pw)` — защита хоста; `ServerEndpoint::SetClientTimeoutMs`.

---

## 5. Клиентский слой `net::ClientEndpoint` + `net::Session`

- `net::ClientEndpoint::Connect(ip, port)` → ENet host + peer.
- `Session::SetPassword(pw)` — задаёт пароль; отправка `PID_AUTH` отложена до
  момента, когда ENet-соединение установлено (`IsPeerReady()`), через
  `Session::SendAuthIfDue()` в `Update()` — иначе пакет до рукопожатия теряется.
- `net::ClientEndpoint` сам шлёт `PID_PING` с заданной частотой (heartbeat), по
  `PONG` считает RTT → `GetPingMs()`/`HasMeasuredPing()`.
- `Session` при приёме `WELCOME` запоминает `selfId`; `PLAYER_LIST` → перестроение
  реестра пиров (`GetPeerRoster`); `PLAYER_STATE` от других → `IRemoteStateSink`
  (через `GamePlayerBridge` в игру); собственный эхо игнорируется по `selfId`.
- Локальное состояние шлётся через `ILocalStateProvider` с каденцией
  `SetStateSendEvery` (по умолчанию каждая 6-я `Update`, ~10 Гц).

---

## 6. Синхронизация машин (A2)

**Ключевое правило повторного использования:** транспортные данные переносятся
**внутри `PLAYER_STATE`**, отдельный `PACKET_VEHICLE_STATE` объявлен, но не
задействован. Дублировать его не нужно.

### Отправляющая сторона (`GameplayLocalStateProvider` в gameplaycontext.cpp)
- Читает `Avatar`: позиция, поворот, `IsInCar()`.
- Если в машине: `vehicleModel = vehicle->GetName()`,
  `netVehicleId = mVehicleCentralIndex` (или хэш указателя),
  `vehicleSpeed = vehicle->GetSpeedKmh()`.
- Если пешком: `vehicleModel[0]=0`, `netVehicleId=0`, `inVehicle=0`.

### Принимающая сторона (`RemoteVehicleWorldView`)
- Sink-абстракция (spawn/move/despawn), как у `RemotePlayerWorldView`,
  тестируема юнитами. Реальную работу с миром делает gameplay-контекст.
- `SyncDriver(playerId, state)`:
  - пешком и нет спавна → не спавнить;
  - вошёл в машину → спавн «призрака» модели;
  - модель/id сменились → деспавн старого + спавн нового;
  - вышел → деспавн.
- `Prune(players)` — деспавн машин водителей, пропавших из авторитетного набора.
- `Update(dt)` — интерполяция позиции (тот же механизм, что у пеших).

- `Update(dt)` также сглаживает `vehYaw` по кратчайшей дуге; руль/скорость/
  состояние/флаги передаются в move-sink как есть.

### Sink в gameplay-контексте (`RemoteVehicleSpawn/Move/Despawn`)
- Спавн через `VehicleCentral::AddNetworkGhostVehicle(model)` — «сетевой
  призрак»: в сцене и коллизии (рендерится и блокирует), но **не** в
  active-list и без типа, который чистит какой-либо менеджер (трафик,
  припаркованные, ресет миссии, husk). Никто, кроме сети, его не освободит.
- Move — кинематическое зеркало: `SetTransform` (позиция + `vehYaw`), затем
  повторно выставляется скорость (SetTransform её обнуляет — иначе колёса не
  крутятся), руль, огни тормоза/заднего хода.
- Despawn — `RemoveNetworkGhostVehicle` (финальный `Release()` сам удаляет
  объект). **Не** вызывать `delete` — двойное освобождение крашило хост при
  двух удалённых водителях.

---

## 7. Игроки: список и уведомления (C8/C9)

- `net::Session` на принимающей стороне при каждом `PLAYER_LIST` перестраивает
  авторитетный реестр пиров (`GetPeerRoster`); события join/leave доставляются
  через `SetJoinLeaveFn(peerId, joined)` на уровне сессии.
- Игровой слой через `NetworkMatchManager::DrainJoinLeaveEvents(out)` забирает
  накопленные `JoinLeaveEvent{clientId, joined}` для UI/консоли.
- В игровом цикле `gameplaycontext` печатает «net: player N connected /
  disconnected» в консоль.
- Консольная команда `match status` выводит состав удалённых игроков через
  `ForEachPlayer`.
- `KeepOnly` в `RemotePlayerManager` применяет авторитетный список.

---

## 8. Дедикат-сервер (D10/D11)

`src/tests/mp_standalone.cpp` — headless-сервер и join-клиент без данных игры.

Использование:
```
mp_standalone server [port] [--mod lua] [--password pw] [--max slots] [--log file]
mp_standalone host   [port] [--password pw] [--max slots] [--log file]
mp_standalone join   <hostIp> [port] [--password pw]
```

- Работает на движковом слое `net::Session` (вместо старого `NetworkServer`/`Client`).
- `--password` → `session.SetHostPassword()` / `session.SetPassword()`.
- `--log` → файл лога (строка запуска + `clients:N` при смене состава).
- `--mod` → принимается, но примечание: Lua-моды на движковом слое не
  подгружаются (см. §9); признак оставлен для совместимости интерфейса.

### CLI основной игры (`CommandLineOptions`)
- `HOSTPORT=<port>` — автостарт хоста.
- `JOINIP=<ip[:port]>` — автостарт join.
- `HOSTPASS=<pw>`, `HOSTSLOTS=<n>`, `JOINPASS=<pw>` — конфигурация.
- Опции геймплея (не сетевые, по умолчанию выкл.): `FREEROAM` (уровень в
  Sunday Drive без миссии), `ALLOWCARFLIP` (авто-переворот машины),
  `CINEMATICJUMPS` (кинокамера прыжков), `EXITCARFADE` (старый выход из машины
  с затемнением и телепортом).
- Все эти опции применяются в `GameplayContext::OnStart`, затем
  `ClearNetworkOptions()` очищает их, чтобы не перезапустить матч повторно.

---

## 9. Lua-моды (`ServerModManager`)

- Загружается один скрипт: `LoadScript(path)`.
- Хуки: `on_join(clientId)`, `on_validate(clientId, x,y,z,speed)`,
  `on_leave(clientId)`.
- Сервер вызывает их в точках допуска/валидации/отвала (см. §4).
- Без скрипта поведение = встроенный авторитетный сервер.
- Тестовый скрипт: `src/tests/test_server_mod.lua`.

---

## 10. Оркестратор `NetworkMatchManager`

Единственный сетевой вход из геймплея. С момента миграции это **тонкий мост**
поверх движкового слоя: он держит `net::Session` + `net::GamePlayerBridge` и
сохраняет прежний публичный АПИ, чтобы потребители (геймплей, консоль, миссии,
машины) не менялись.

- **Host** играет как полноценный участник: `StartHost(port)` запускает сервер и
  сразу подключает loopback-клиент к своему же порту (`127.0.0.1:port`). Поэтому
  host одновременно держит `m_server` + `m_client` и видим другим клиентам (его
  локальное состояние ретранслируется как у любого присоединившегося).
  `HostAndJoinAreMutuallyExclusive` по-прежнему запрещает *явный* join, пока идёт
  матч, но сам host себе подсоединяется через loopback.
- **Join-клиент** — только `m_client` (`JoinHost`).

- `StartHost(port)` / `JoinHost(ip, port)` / `Leave()` — жизненный цикл матча
  (делегирует `net::Session`).
- `SetHostConfig(password, maxPlayers)` — конфиг; пароль host-сервера зеркалится
  на loopback-клиент, чтобы host прошёл тот же авторизационный гейт.
- `SetLocalStateProvider(fn)` — провайдер локального состояния (`GamePlayerBridge`).
- `GetRemotePlayers()` — реестр **со стороны клиента** (для host и для
  join-клиента), т.к. клиентская перспектива исключает самого игрока (`selfId`)
  из remote-набора; голый серверный реестр включал бы host самому себе.
- `DrainJoinLeaveEvents(out)` — уведомления join/leave.
- `Update()` — крутит `net::Session::Update()` (сервер+клиент+релей+валидатор).

---

## 11. Визуализация с интерполяцией (`RemotePlayerWorldView`)

- Sink: `spawn`/`move`/`despawn` оторваны от мира (тестируемо).
- `Sync(players)` — обновляет цели (target) для каждого удалённого игрока,
  спавнит/деспавнит по авторитетному составу.
- `Update(dt)` — покадровое экспоненциальное сглаживание к цели
  (frame-rate independent, `lambda = 3/smoothingSeconds`), не телепортирует.
- Ключевой договор: изменение позиции НЕ двигает модель в `Sync` — только
  задаёт цель; реальное движение происходит в `Update`. Это убирает рывки и
  джиттер на частоте пакетов.
- Фейсинг (`rotY`) сглаживается по кратчайшей дуге (350° → 10° через 0).
- Поля локомоушна (`motionAngle`/`facingAngle`/`speedFactor`/`characterState`,
  turbo) передаются в move-sink без предсглаживания: в игре при
  `characterState == LOCO` ими управляет `LocomotionDriver` пуппета
  (`SetFacingDir` + скорость от `GetMaxSpeed()`).
- `action`-sink вызывается один раз на rising-edge ненулевого `actionEvent`
  (`RemotePlayerAction`: `ACTION_JUMP` → `RemotePlayerJump`).

---

## 12. Синхронизация миссии (A3, co-op)

Миссии становятся **совместными только когда игроки проходят их вместе**, а не
принудительно для всех. Механика — релей прогресса миссии, как у `PLAYER_STATE`:

- **Состояние:** `MissionSyncState` (миссия/стадия/цель + флаг `inMission`),
  сериализация как у `PlayerNetworkState`.
- **Сервер:** ретранслирует `PID_MISSION_STATE` остальным (без gate-валидации —
  ход миссии решает хост).
- **Клиент:** `GetRemoteMissionStates()` отдаёт `map<clientId, MissionSyncState>`
  для UI «кто в какой миссии».
- `MissionSyncManager` агрегирует локальный + удалённые прогрессы, тестируем.

- **Движковый канал:** отдельный `PID_MISSION_STATE` в `net::Session`.
  Провайдер — `IMissionStateProvider` (`WorldInterfaces.h`), sink —
  `SetMissionStateSink`. Отправка на своей, более редкой каденции
  (`SetMissionSendEvery`, по умолчанию раз в 18 кадров ≈ 3,3 Гц). Сервер
  ретранслирует `[sender][payload]` всем, кроме отправителя, без валидатора.
- `NetworkMatchManager::AttachMissionChannel()` связывает `GameMissionAdapter`
  с сессией; удалённые прогрессы — `GetRemoteMissionStates()` /
  `GetRemoteMissionState(peerId, out)`. Тест — `MissionStateRelaysThroughDedicatedChannel`.

### 12.1 События мира (`WorldEvent`, релей через `PID_USER`)

Разовые изменения мира идут через `WorldEventCodec` (заголовок 6 байт +
POD-полезная нагрузка без игровых типов). Эмиттеры — `src/game/network/*WorldEventEmitter.*`,
безопасный no-op вне матча:

| Тип | Эмиттер / точка вызова | Payload |
|---|---|---|
| `WE_STATEPROP` 0x01 | `EmitStatePropWorldEvent` — `StatePropDSG` (смена состояния) | uid + state |
| `WE_BREAKABLE` 0x02 | `EmitBreakableWorldEvent` — `StatePropDSG` (разрушение) | persistentId + xyz |
| `WE_VEHICLE_DESTROYED` 0x03 | `EmitVehicleDestroyedWorldEvent` — `vehicle.cpp` (взрыв) | netVehicleId + xyz |
| `WE_COIN_COLLECTED` 0x04 | `EmitCoinCollectedWorldEvent` — `CoinManager::CheckCollection` | persistentId + xyz |
| `WE_PLATFORM_DIR` 0x05 | `EmitPlatformDirWorldEvent` — `animcollisionentitydsg.h` | uid + direction |

Приёмник (`gameplaycontext.cpp`, `SetWorldEventSink`) сейчас применяет только
`WE_VEHICLE_DESTROYED`: находит локальную машину по net id и повторно
шлёт `EVENT_VEHICLE_DESTROYED` (husk-логика). Призраки удалённых водителей
не трогаются — их снимает поток drive-state. Остальные типы пока только
передаются (приём — следующий шаг, см. `PRECISE_MULTIPLAYER_PLAN.ru.md`).

## 13. Точки интеграции в игровой цикл (`gameplaycontext.cpp`)

Порядок в `GameplayContext::OnUpdate` внутри матча:
1. `pMatch->Update()` — сеть (и отправка состояния миссии через месседж-провайдер).
2. `GetRemotePlayerWorldView()->Sync(...)` → `Update(elapsedTime)` — пешие.
3. `ForEachPlayer` → `RemoteVehicleWorldView::SyncDriver` → `Prune` → `Update` — машины.
4. `DrainJoinLeaveEvents` → консольные сообщения.

В `OnStart`: автостарт матча из CLI, `SetSink` для обоих WorldView'ов,
`SetMissionStateProvider` для локального прогресса миссии.

---

## 13. Совместимость: тесты (правило проекта)

- Все изменяемые возможности должны иметь юнит/интеграционные тесты
  (`src/tests/network_tests.cpp`; проект Tests.vcxproj, пакет `Tests.dll`).
- Прогон: `vstest.console.exe` по `Tests.dll`. Текущее состояние — 42/42 зелёных.
- Сборка полной игры: `MSBuild.exe src/srr2/SRR2.vcxproj
  /p:Configuration=DebugWindows /p:Platform=x64
  /p:WindowsTargetPlatformVersion=10.0.22621.0`.
  - **Важно:** проект `sim.vcxproj` целится в SDK 10.0.26100.0, которого нет на
    машине; версию SDK нужно переопределять в командной строке
    (`/p:WindowsTargetPlatformVersion=10.0.22621.0`). Тесты собираются отдельно:
    `MSBuild.exe src/tests/Tests.vcxproj "/p:Configuration=Debug" "/p:Platform=x64"`.

---

## 14. Вход в мультиплеер из главного меню (C7)

**Интеграция без генерации нового frontend.p3d** — пункт «MULTIPLAYER»
добавляется кодом в существующее главное меню (`CGuiScreenMainMenu`).

- `guiscreenmainmenu.h`: в `eMainMenuSelection` вставлен `MAIN_MENU_MULTIPLAYER`
  (теперь пункт между `MAIN_MENU_MINI_GAME` и `QUIT_GAME`). Строка подписи
  назначается в `InitMenu()` через `mainMenuText->SetString(MAIN_MENU_MULTIPLAYER, "MULTIPLAYER")`
  (в TextBible для нового слота нет локализованной строки).
- Выбор пункта (`MAIN_MENU_MULTIPLAYER` в `GUI_MSG_MENU_SELECTION_MADE`) →
  `OnMultiplayerSelected()` → `StartMultiplayerOverlay()`.
- В оверлее `GUI_MSG_CONTROLLER_UP/DOWN` переключают `Host`/`Join`,
  `GUI_MSG_CONTROLLER_SELECT` подтверждает (`OnMpOverlayConfirm` → Host/Join).

### Ввод IP через `TextInputHook`
- `src/game/presentation/gui/utility/textinputhook.{h,cpp}` — глобальный хук
  текстового ввода: `SetActive(OnChar, OnBackspace)`/`ClearActive()`,
  `HasActiveConsumer()`. Единственный активный потребитель.
- В `win32platform.cpp` в SDL-цикл вставлены вызовы: `SDL_TEXTINPUT` →
  `TextInputHook::ReportText(...)`, `SDLK_BACKSPACE` → `ReportBackspace()`.
- Экран меню регистрирует статик-обработчики `OnMpOverlayChar`/`OnMpOverlayBackspace`,
  буфер `m_mpIpBuffer[64]`; `UpdateMpOverlayDisplay()` рисует промпт Host/Join + IP.

### Переход в геймплей MP (host/join) — важные подводные камни
`StartMultiplayerHost`/`StartMultiplayerJoin` должны полностью повторить путь
«New Game», иначе мир/персонаж/контроллер не готовы:
1. `NetworkMatchManager::StartHost(port)` / `JoinHost(ip, port)` — сеть (до входа
   в геймплей; сервер/клиент стартует сразу).
2. `GetInputManager()->RegisterControllerID(0, 0)` + `SetPrimaryController(0)` —
   без этого `GetControllerIDforPlayer(0)` == -1 → краш в
   `guimanageringame.cpp` (вход в геймплей) и нет управления персонажем.
3. `if (IsGameLoaded()) ResetGame() + ClearRewards()` — сброс загруженного
   сохранения, иначе при выходе из игры ассерт `simutility.cpp:153`
   (SimEnvironment не выгрузился из-за посторонних ph-объектов сохранения) и
   не работает клавиша взаимодействия.
4. `GetCharacterSheetManager()->SetCurrentMission(L1, M1)` — без этого мир
   загружается без спавна играбельного персонажа (нет управления после кат-сцены).
5. Вход — штатным `GUI_MSG_QUIT_FRONTEND, 1` (он ставит `GameplayManager`);
   прямой `SetContext(CONTEXT_LOADING_GAMEPLAY)` НЕ использовать — в этом
   контексте `GetGameplayManager()` ещё NULL → AV.

### Защита от выхода контроллера за границы (деградация)
- `humanvehiclecontroller.cpp`: `GetController(mControllerId)` в
  `GetSteering`/`Update` обёрнут в `if (mControllerId >= 0 && < MaxControllers)`.
- `guimanageringame.cpp` (опрос подключения): guarded `controllerID != -1`.
- Все вызовы `GetController` в этих местах больше не триггерят
  `rAssert(controllerIndex < MaxControllers)` в `inputmanager.cpp`.

### Краш-хендлер (`win32main.cpp`)
- `SetUnhandledExceptionFilter(CrashHandler)` + хуки `abort`/`terminate`/
  pure-virtual (`_set_purecall_handler`, `set_terminate`, `signal(SIGABRT)`).
- Пишет символьный стек (DbgHelp) в `crash.log` в рабочей папке, не полагаясь
  на окно ошибки. `WriteCrashDump(reason)` — доступен для точечной диагностики
  (временный вызов в `GetController` при out-of-range индексе).

### Известный недочёт
Оверлей ввода IP — это отдельный видимый трёхстрочный промпт
(`MULTIPLAYER HOST` / `Address: IP` / `[Enter=... Esc=back]`), нарисованный на
скрытом тексте `MainMenu` страницы (заданы `SetBoundingBoxSize`/`SetPositionOfCenter`
и `SetIndex(0)`; `UpdateMpOverlayDisplay` пишет через `SetString(0, buf)`).
Пункты меню не трогаются, поэтому навигация не ломается. Открытие оверлея идёт
через `GUI_MSG_MENU_SELECTION_MADE`, а не через SELECT в обработчике, поэтому
`m_mpOverlayIgnoreFirstSelect` был удалён — Enter подтверждает с первого нажатия.
Пустой IP при Host = дефолтный порт 3737; при Join с пустым адресом join не
стартует (бесшовно возвращается). Визуально это всё ещё не диалоговое окно с
полем ввода, а промпт поверх меню — дальнейшая проработка в TODO.

---

## 15. План (TODO, вернёмся)

- **C7 (UI-лобби, улучшение)** — сделать отдельное видимое поле ввода IP/порта
  в оверлее Host/Join (сейчас это промпт внутри текста пункта меню). Возможно,
  своими экранами после генератора .p3d / UI-слоя.

**MP-стабильность — проверено end-to-end (2026-10, после миграции на net::*):**
- **Сеть на реальных сокетах подтверждена.** Автономный стенд
  `src/tests/mp_standalone` (на `net::Session`/`net::*`) сводит host+join как
  два процесса: сервер фиксирует `clients in world: 1`, join получает
  `SERVER READY, my client id = 2`. Дополнительная изоляция: in-game join-копия
  подключается к `mp_standalone`-серверу (`selfId=2`) — значит in-game
  клиентская сеть исправна.
- **Полный in-game host↔join (две копии SRR2 в геймплее)** на этой машине НЕ
  сходится на уровне ОС, а не сетевого кода: оба процесса обслуживают свои
  ENet-сокеты (маркеры подтверждают опрос и приём), серверный UDP-сокет жив на
  `0.0.0.0:<порт>`, CL-коннект корректно создаётся, но ENet-рукопожатие между
  двумя инстансами не завершается даже при выключенном VPN и файрволе
  (Private/Public выключены). Это ограничение среды запуска двух headless-копий
  SRR2 на данной машине, не дефект `net::` слоя.
- **Ручной вход host/join** через главное меню (видимые окна) остаётся
  рекомендуемым способом проверить управление персонажем; логирование сети
  через временные маркеры снято после диагностики, `net::` слой оставлен чистым.

### Известная проблема (ПОТЕНЦИАЛЬНАЯ, не подтверждена как дефект кода)
**Полный in-game host↔join между двумя копиями SRR2 не завершает ENet-рукопожатие.**
2026-10, одна машина. Признаки: оба процесса обслуживают свои ENet-сокеты
(опрос и приём подтверждены маркерами), UDP-сервер слушает `0.0.0.0:<порт>`,
клиентский `enet_host_connect` создаёт peer корректно — но приём внешнего пира
сервером не происходит, `selfId` у join остаётся 0. Не помогает: выключенный VPN,
выключенные профили файрвола (Private/Public), видимые окна вместо headless,
разные порты, последовательный старт. Контраст: тот же `net::` слой корректно
работает на реальных сокетах в `mp_standalone` (host+join, сервер видит peers,
join получает `selfId=2`) и in-game join подключается к `mp_standalone`-серверу.
**Нужно проверить:** является ли ограничением запуска двух headless-копий SRR2
на данной машине, средовой блокировкой (Windows Filtering Platform/driver) или
реальным дефектом `net::` слоя, — повторить на машине без сетевых стеков/драйверов
третьих сторон, либо перепроверить на другой машине.

**Зафиксировано (не баг сети):** редкий разовый флейк «загрузка мира застревает
почти у конца» при входе хостом через меню. Расследование: сетевой
`NetworkMatchManager::Update` неблокирующий (`enet_host_service` с timeout 0),
вызывается только после загрузки мира (gameplaycontext.cpp:785), к зависанию
отношения не имеет. `skipfe HOSTPORT=3737` (тот же сетевой старт) и обычный
старт за 7+ запусков проходили чисто (resp, no crash); краши `0xc0000005` в
системном журнале — от старых сборок. Вывод: это интермиттентный флейк
загрузки уровня (лечится перезапуском), не мультиплеерный.

---

### Авто-драйвер UI для тестов (отключаемый)
В `CGuiSystem::Update` (guisystem.cpp) встроен **тест-харнесс фронтенда**: при
`SRR2_AUTOMENU=1` сам жмёт `SELECT`/`BACK`/`DOWN` по таймеру, логируя шаг в
`mp_dbg.log`. По умолчанию **выключен** (`getenv` == NULL) и на геймплей не
влияет. Назначение — автоматический обход меню/экранов для поиска крашей без
ручного тестирования. ВКЛЮЧАТЬ только для отладки; при старте нужен ворт-ап
(чтобы сработал после загрузки фронтенда).

### Краш-хендлер vs rAssert (важно)
`CrashHandler` (win32main.cpp) через SEH ловит access violation (`0xc0000005`)
и через хуки — `abort`/`terminate`/pure-virtual call → пишет стек в `crash.log`.
**Но `rAssert`/`rAssertMsg` НЕ пишут crash.log**: при срабатывании в Debug
`rDebugAssertFail_Implementation` (radcore raddebug/debug.cpp) при нажатии
Abort вызывает `exit(-10)` — это не SEH и не попадает в наши хуки, стек в
`crash.log` не фиксируется (только текст в консоли). Пробел: rAssert-ассерты не
дампятся. Если нужно — подключить перехват Abort-ветки ассерта для записи
`crash.log`.

---

## 16. Правило на будущее

**Перед любой разработкой и в ходе — сверяйся с этим документом.** Если фича
уже описана, не создавай её заново и не добавляй дублирующий транспорт/пакет
(пример: `PACKET_VEHICLE_STATE` объявлен, но данные машин живут в
`PLAYER_STATE` — не плоди второй путь). После крупных изменений обновляй этот
файл.