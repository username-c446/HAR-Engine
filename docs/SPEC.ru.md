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

Сетевой слой разделён на независимые части: переиспользуемый движковый слой
(`src/game/netengine/`), игровой слой (`src/game/network/`) и продукт «выделенный
сервер» (`src/server/`). Старый монолит `NetworkServer`/`NetworkClient` удалён.

### Движковый слой `src/game/netengine/` (переиспользуемый, world-agnostic)

| Файл | Роль |
|------|------|
| `Packet.h` | Протокол: `PacketId`, `PROTOCOL_VERSION`/`PROTOCOL_MAGIC`, причины отказа, каналы ENet + доставка по id (`ChannelOf`), `Writer`/`Reader` с проверкой границ |
| `PacketRegistry.{h,cpp}` | Реестр пакетов (id → обработчик) |
| `ServerEndpoint.{h,cpp}` | Чистый ENet-хост: `PeerId`, фрейминг, таймаут, `KickPeer` (сначала доставляет очередь), серверные часы `NowMs`, PONG с серверным временем, RTT пира |
| `ClientEndpoint.{h,cpp}` | Чистый ENet-клиент: фрейминг, heartbeat по времени, RTT и оценка серверных часов (`ClockSync`) |
| `NetTime.h` | `ClockSync`, `SnapshotBuffer<T>` (буфер джиттера), `TokenBucket` |
| `ServerCore.{h,cpp}` | **Авторитетный сервер** (одна реализация для игры-хоста и выделенного сервера): рукопожатие, допуск, мир, леджер, релеи, валидация, лимиты, опции сессии, истечение передачи владения |
| `Session.{h,cpp}` | Участник матча: всегда клиент; при хостинге дополнительно крутит встроенный `ServerCore` и подключается к нему через loopback |
| `WorldState.{h,cpp}` | Реплицируемый мир: таблица / зеркало / кодеки, передача владения (§17, §18.5) |
| `Ledger.{h,cpp}` | Серверные счётчики (§17.3) |
| `GamePlayerBridge.{h,cpp}` | Игровой адаптер: `PlayerNetworkState` ↔ непрозрачные блобы сессии |
| `WorldInterfaces.h` | `ILocalStateProvider`/`IRemoteStateSink`/`IStateValidator`/`IMissionStateProvider` |
| `ENetInit.{h,cpp}` | RAII-счётчик ссылок ENet |

### Игровой слой `src/game/network/`

| Файл | Роль |
|------|------|
| `NetworkPacket.h` | Little-endian примитивы (`net::Write*/Read*`) |
| `PlayerNetworkState.{h,cpp}` | Структура состояния игрока + сериализация (§3) |
| `GameServer.{h,cpp}` | Игровые правила сервера поверх `ServerCore` (экономика, валидация движения, Lua-моды, PvP) — общие для игры-хоста И выделенного сервера |
| `PlayerMovementValidator.{h,cpp}` | Серверная проверка правдоподобия движения (§18.3) |
| `NetworkMatchManager.{h,cpp}` | Оркестратор для геймплея (§10) |
| `RemotePlayerManager.{h,cpp}` | Реестр удалённых игроков (состояние + серверное время сэмпла) |
| `RemoteStateTrack.h` | Snapshot-интерполяция одного удалённого игрока (§11) |
| `RemotePlayerWorldView.{h,cpp}` | Удалённые пешие игроки в мире |
| `RemoteVehicleWorldView.{h,cpp}` | Машины-призраки удалённых водителей |
| `NetWorldRegistry`, `NetEntityManager`, `PlayerEconomy`, `NetWorldHooks`, `WorldObjectId.h`, `EconomyConfig.h` | Реплицируемый мир (§17, §18) |
| `ServerModManager.{h,cpp}` | Серверные Lua-моды |
| `MissionSyncManager`/`MissionSyncState` | Прогресс миссий (co-op) |
| `NetworkConsoleCommands.{h,cpp}` | Консольные команды `host`/`join`/`match` |

### Выделенный сервер `src/server/` (продукт)

`HarServer.vcxproj` → `har_server.exe` (§8).

Интеграция в игру: `src/game/contexts/gameplay/gameplaycontext.cpp` (провайдеры,
sink'и, вызовы в игровом цикле) и `src/game/main/commandlineoptions.{h,cpp}`
(автостарт из CLI).

---

## 2. Сетевая модель и транспорт

- **Транспорт:** ENet (vcpkg `enet`), три канала (`Packet.h`):
  - `CH_RELIABLE` (0) — надёжный упорядоченный: рукопожатие, мир, леджер,
    события, пользовательские данные;
  - `CH_STREAM` (1) — ненадёжный последовательный: потоки «последнего
    состояния» (`PID_PLAYER_STATE`, `PID_MISSION_STATE`, `PID_ENTITY_STATE`,
    `PID_USER_STREAM`). Потерянный пакет потока заменяется следующим, а не
    задерживает всё за ретрансмитом; опоздавший старый пакет ENet отбрасывает;
  - `CH_TIME` (2) — ненадёжный: `PID_PING`/`PID_PONG` (ретрансмит исказил бы RTT).
  Канал выбирается по id пакета в одном месте (`ChannelOf`), для обоих эндпоинтов.
- **Авторитетный сервер**, никакого P2P. Сервер допускает клиентов, выдаёт
  `PeerId`, валидирует, ретранслирует и арбитрирует; клиенты предсказывают, сервер
  решает (§17).
- `MAX_PACKET_PAYLOAD = 512` байт; фрейминг `[u16 id][u16 size][payload]`.
- Живость: клиент пингует каждые 500 мс (`SetPingIntervalMs`); сервер отключает
  пира, молчащего 30 с (`SetClientTimeoutMs`).

### Id пакетов (`Packet.h`, `PROTOCOL_VERSION = 3`)

| Id | Имя | Направл. | Назначение |
|----|-----|----------|------------|
| 0x00 | `HELLO` | C→S | `u32 magic, u16 protocol, u32 версия игры, u8 n, пароль` |
| 0x01 | `WELCOME` | S→C | `u16 peer id, u32 время сервера, u32 опции сессии` |
| 0x03 | `REFUSE` | S→C | `u8 причина, u16 протокол сервера`, затем отключение |
| 0x04 | `PLAYER_LIST` | S→C | допущенные пиры |
| 0x05/0x06 | `PING`/`PONG` | C↔S | `{время клиента}` / `{эхо, время сервера}` |
| 0x07 | `WORLD_SEED` | S→C | `u32` |
| 0x08 | `SESSION_INFO` | S→C | опции сессии изменились |
| 0x09 | `STATE_CORRECTION` | S→C | валидатор отклонил наше состояние: встать в этот блоб |
| 0x10–0x15 | `WS_*` | | реплицируемый мир (§17.1) |
| 0x20/0x21 | `LEDGER_*` | | счётчики (§17.3) |
| 0x100 | `PLAYER_STATE` | C→S→C | поток игрока (с меткой времени) |
| 0x102 | `MISSION_STATE` | C→S→C | поток прогресса миссии |
| 0x103 | `ENTITY_STATE` | C→S→C | поток сущностей с проверкой владельца |
| 0x104 | `ENTITY_EVENT` | C→S→владелец | удар по сущности (§18.6) |
| 0x105 | `PLAYER_EVENT` | C→S→жертва | PvP-удар (§18.7) |
| 0x8000/0x8001 | `USER`/`USER_STREAM` | C→S→C | данные модов, надёжно / потоком |

Фрейминг потоков: клиент → сервер `[u32 серверное время сэмпла][payload]`;
сервер → остальным `[u16 отправитель][u32 время][payload]`. Сервер зажимает время в
`[now − 2000 мс, now]` (клиент не может заявить будущее).

---

## 3. Состояние игрока `PlayerNetworkState`

Точная раскладка байт (порядок полей важен для сериализации):

```
struct PlayerNetworkState {
    float x, y, z;            // позиция
    float rotY;               // поворот вокруг вертикали
    char  vehicleModel[16];   // имя модели машины ("" если пешком)
    int32 netVehicleId;       // стабильный сетевой id машины
    float vehicleSpeed;       // скорость машины (км/ч)
    uint8 inVehicle;          // 1 = в машине
    uint8 flags;              // FLAG_JUMPING | FLAG_IN_AIR | FLAG_LOCO_TURBO
    // --- непрерывная локомоция (пешком) ---
    uint16 motionAngle;       // направление движения, рад*1000
    uint16 facingAngle;       // направление взгляда, рад*1000
    uint8  speedFactor;       // 0..255 доля максимальной скорости
    uint8  characterState;    // CharacterAi::CharacterState (LOCO и т.п.)
    // --- разовое действие ---
    uint8  actionEvent;       // [seq:3 | ACTION_* id:5] последнее действие; 0 = нет
    // --- кинематическое зеркало машины (inVehicle) ---
    float  vehYaw;            // курс машины, рад
    uint8  vehSteer;          // руль -128..127 -> -45..+45 град
    uint16 vehSpeedKmh;       // скорость, км/ч
    uint8  vehState;          // VS_NORMAL / VS_SLIP / VS_EBRAKE_SLIP
    uint8  vehFlags;          // VEH_FLAG_BRAKE | VEH_FLAG_REVERSE | VEH_FLAG_DESTROYED
    uint8  vehDamage;         // повреждение машины 0 (целая) .. 255 (разбита)
    uint8  teleportSeq;       // растёт при каждом намеренном разрыве позиции
};
```

- Формат на проводе (протокол 3; `Serialize` возвращает размер, `Deserialize(data,
  size)` отвергает обрезанную / слишком длинную запись и неизвестный бит секции).
  Передаётся только то, что игрок делает сейчас:
  ```
  u8  sections            SEC_LOCO 0x01 (пешком) | SEC_VEHICLE 0x02 (в машине)
  i24 x, y, z             сантиметры (+-83,88 км, с ограничением; NaN -> 0)
  u16 rotY                угол 2pi/65536, декодируется в (-pi, pi]
  u8  flags | u8 characterState | u8 actionEvent | u8 teleportSeq
  [SEC_LOCO]    u16 motionAngle | u16 facingAngle | u8 speedFactor
  [SEC_VEHICLE] i32 netVehicleId | u16 vehYaw | u8 vehSteer | u16 vehSpeedKmh
                | u8 vehState | u8 vehFlags | u8 vehDamage
  ```
  Пешком 21 байт, в машине 28 (`MAX_SERIALIZED_SIZE`); протокол 2 слал всегда 60.
  Поля отсутствующего блока декодируются как 0; `vehicleSpeed` восстанавливается
  из `vehSpeedKmh` (отправитель заполнял оба из `GetSpeedKmh`). Общие
  функции фиксированной точки — `netengine/Quantize.h` (`WriteCentimetres24`,
  `EncodeAngle16`, ...).
- В машине поля локомоции/действия обнулены; пешком — поля машины.
- `actionEvent` приходит из `net::LocalActionLatch` (`LocalActionLatch.h`): точки
  действий локального аватара зовут `Report(id)`; защёлка держит последнее действие
  с 3-битной последовательностью (`MakeActionEvent`/`ActionId`). Приёмник
  срабатывает на изменение байта. Источники: `state.cpp` (DODGE, CRINGE, ATTACK,
  CELEB_SMALL/BIG, FLAIL в `InSim::Enter`), `action.cpp` (SLAM, JUMPKICK, GETUP).
  Прыжки идут через `FLAG_JUMPING`. `ACTION_KD`/`ACTION_SURF` зарезервированы.
- `vehDamage` = `(1 − Vehicle::GetHealthFraction()) · 255`; приёмник показывает его
  на машине-призраке (`Vehicle::ApplyNetworkDamage`).
- `teleportSeq`: игра телепортирует аватара из многих мест, поэтому провайдер
  определяет это сам (`TrackLocalTeleport` в `gameplaycontext.cpp`): скачок между
  двумя сэмплами, недостижимый ни для машины (110 м/с), ни бегом (20 м/с), плюс
  25 м. Серверная коррекция не считается (сервер эту позу уже знает). Сервер
  пропускает скачок только вместе с новым значением и с ограничением частоты (§18.3).
- `vehicleModel` не передаётся в потоке: это запись присутствия `WOC_VEHICLE`
  (§18.11), при приёме подставляется обратно; вместе с `netVehicleId` идёт в
  `RemoteVehicleWorldView`, и приёмник создаёт призрака той же модели (пока
  запись не пришла, для вида машин игрок считается пешим).

---

## 4. Сервер: `net::ServerCore`

Единственная реализация авторитетного сервера (`netengine/ServerCore.h`). Игра-хост
крутит его внутри своей `Session` (встроенный) и подключается к нему через
loopback; продукт «выделенный сервер» (§8) запускает тот же класс отдельно.
Поэтому матч на хосте и на выделенном сервере ведёт себя одинаково.

### Рукопожатие и допуск
1. ENet connect → `ServerEndpoint` выдаёт `PeerId` (с 1). Пир НЕ допущен: от него
   принимается только `PID_HELLO`, и он ничего не получает о мире (рассылки мира и
   списка игроков идут только допущенным).
2. `PID_HELLO` проверяется по порядку: magic + `PROTOCOL_VERSION` → версия игры
   (`SetGameVersion`, 0 = не проверять) → пароль (`SetPassword`, пусто = открытая
   сессия) → лимит слотов (`SetMaxPeers`, loopback хоста учитывается) → фильтр
   допуска (`SetAdmissionFilter`, например `on_join` Lua-мода). Любой отказ
   отправляет `PID_REFUSE{причина}` (`REFUSE_PROTOCOL`, `_GAME_VERSION`,
   `_PASSWORD`, `_FULL`, `_DENIED`) и отключает пира после доставки отказа.
3. Пир, не приславший верный HELLO за `SetHandshakeTimeoutMs` (5 с), получает
   `REFUSE_HANDSHAKE`.
4. Допуск: `WELCOME{peer, время сервера, опции}` + `WORLD_SEED` + `PLAYER_LIST`
   всем + снапшот мира (надёжный упорядоченный канал: весь мир приходит раньше
   любого последующего изменения).
5. Ёмкость транспорта ENet = лимит слотов + 4, чтобы клиент сверх лимита получал
   `REFUSE_FULL`, а не таймаут.

### Лимиты частоты (`RateLimitConfig`, `SetRateLimits`)
На каждого допущенного пира — token bucket по классам: потоки (90/с, всплеск 180),
запросы (мир / леджер / события: 240/с, всплеск 600 — новичок сбрасывает очередь
разом), надёжные пользовательские пакеты (60/с, всплеск 120) и байты (96 КиБ/с,
всплеск 192 КиБ). Пакет сверх бюджета отбрасывается; 300 отбросов за 10 с →
`REFUSE_FLOOD` и кик. `GetStats()` считает отказы, отбросы, отказы валидатора,
коррекции и кики за флуд.

### Релей и валидация
- `PID_PLAYER_STATE` проходит `IStateValidator::ValidatePeerState(from, blob,
  serverNow, correction)` (§18.3): `SV_ACCEPT` — переслать, `SV_DROP` —
  отбросить, `SV_CORRECT` — отбросить и отправить `PID_STATE_CORRECTION` с блобом,
  в который клиент должен встать.
- Потоки пересылаются с метками времени (§2); потоки сущностей — только для
  записей, которыми владеет отправитель; события сущностей — владельцу; события
  игрока — жертве при включённом PvP.
- Уход пира: его сущности стираются или освобождаются (§18.5), балансы удаляются,
  валидатор забывает пира, рассылается `PLAYER_LIST`.

### Наблюдатели
`SetJoinLeaveFn` (вход/выход на стороне сервера), `SetWorldChangeObserver` (каждое
принятое изменение/стирание), `SetUserPacketObserver`.

---

## 5. Клиент: `net::Session`

- `Join(ip, port)` → `ClientEndpoint`; как только ENet-соединение установлено,
  уходит `PID_HELLO` с `SetGameVersion` и `SetPassword` (сессия-хост предъявляет
  пароль своего же сервера).
- `GetRefusal()` — почему сервер отказал (сессия при этом вышла из матча).
  `IsConnectionLost()` — установленное соединение оборвалось без отказа. Отказ
  применяется только после выхода из цикла обслуживания транспорта (эндпоинт
  никогда не уничтожается внутри собственного колбэка).
- Серверные часы: начальная оценка из WELCOME, уточнение каждым PONG (`ClockSync`:
  смещение берётся из сэмпла с минимальным RTT среди последних 8; RTT сглажен).
  `ServerTimeMs()`, `GetRttMs()`, `IsClockSynced()`.
- Вход/выход: из разницы `PLAYER_LIST` на каждом клиенте (loopback хоста и
  присоединившиеся одинаково) → `SetJoinLeaveFn` и `IRemoteStateSink::OnPeerLeft`.
  `Leave()` сообщает об уходе оставшихся пиров; деструктор ничего не сообщает
  (sink'и к этому моменту могут быть уже уничтожены).
- Потоки: локальное состояние каждые `SetStateSendIntervalMs` (100 мс), прогресс
  миссии каждые `SetMissionSendIntervalMs` (300 мс), по времени.
- Запросы (мир, леджер, события, надёжные пользовательские данные) ждут в очереди
  (≤ 256, порядок сохраняется) до WELCOME.
- Опции сессии: `GetSessionOptions()` / `SetSessionOptionsFn` (бит `SO_PVP`); при
  хостинге — `SetSessionOptions`.
- События: `SendEntityEvent`/`SetEntityEventSink`, `SendPlayerEvent`/
  `SetPlayerEventSink`; пользовательские данные `SendCustomEvent(data, size, stream)`.
- `SetStateCorrectionSink` получает коррекции сервера.

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
- `SyncDriver(playerId, state, serverTime)` записывает сэмпл с меткой времени;
  `Update` показывает позу по snapshot-интерполяции (§11), `vehYaw` по кратчайшей
  дуге; руль/скорость/состояние/флаги/урон — из ближайшего сэмпла.
- Призрак, которого мир не смог создать (нет свободной области коллизии),
  создаётся повторно со следующим сэмплом.

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
  двух удалённых водителях. Призрак, удалённый, пока его скрипт машины (`.con`)
  ещё грузится, держит одну ссылку до конца постройки
  (`ReleaseDeferredNetworkGhosts`): консольные хуки скрипта пишут в «машину в
  постройке» (освобождённый призрак там давал use-after-free).
- У призраков свой бюджет областей коллизии
  (`WorldPhysicsManager::NETWORK_GHOST_VEHICLES = 16`, в sim
  `CollisionAreaListSize = 37 + 16`); `AddNetworkGhostVehicle` возвращает NULL,
  если свободной нет, вместо release-assert.
- Урон: `Vehicle::ApplyNetworkDamage(1 − vehDamage/255)`; призрак никогда не
  получает локальный урон (`Vehicle::mNetworkGhost`, §18.6).

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

## 8. Выделенный сервер (продукт)

`src/server/HarServer.vcxproj` → `har_server.exe`: голый `net::ServerCore` с
игровыми правилами (`GameServer`: экономика, валидация движения, PvP, серверные
Lua-моды), без игры, окна, ассетов и loopback-игрока — сервер не занимает слот.
Это тот же сервер, что встроен в игру-хост.

```
har_server [port] [--password <pw>] [--max <players>] [--mod <lua>]...
           [--pvp] [--cheats] [--game-version <n>] [--seed <n>]
           [--no-interest] [--stats <seconds>] [--log <file>] [--verbose]
```
- `--stats` / `--no-interest`: отчёт о трафике и отключение фильтрации (§18.11).
- Порт по умолчанию 3737; без пароля — открытая сессия; `--max` — слоты игроков.
- `--mod` можно повторять (по `ServerModManager` на скрипт).
- `--verbose` пишет каждое изменение мира; отказы / кики за флуд / коррекции
  движения логируются из `ServerCore::GetStats()`.
- Ctrl+C / SIGTERM корректно останавливают. Сборка кладёт `lua.dll` рядом с exe.

`src/tests/mp_standalone` теперь только headless ТЕСТОВЫЙ клиент (`join`, `bots`,
`genui`): `join` подключается к игре-хосту или `har_server`, шлёт фейковое
движение и печатает удалённых игроков и полученный снапшот; `bots` —
нагрузочный тест (§18.11).

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

- По менеджеру на скрипт (`LoadScript(path)`); `GameServer` грузит заданный
  список для ОБОИХ видов сервера (игра-хост: `NetworkMatchManager::SetHostMods`;
  выделенный: `--mod`).
- Хуки: `on_join(clientId)` → фильтр допуска (false = `REFUSE_DENIED`),
  `on_validate(clientId, x,y,z, speed)` (speed — измеренные м/с с последнего
  принятого состояния) после правил движения, `on_leave(clientId)`; события
  `player.joined` / `player.left` через `net.on`.
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
- `Update()` — крутит `net::Session::Update()`; если матч закончился сам (отказ /
  обрыв), выходит и сбрасывает сервисы мира; `GetLastRefusal()` /
  `WasConnectionLost()` говорят почему.
- Хостинг подключает `GameServer` к встроенному ядру сессии (`SetHostConfig`,
  `SetHostPvp`, `SetHostMods`, `SetAllowEconomyCheats`).
- PvP и коррекции: `SetStateCorrectionHandler`, `SetPlayerHitHandler`,
  `SetRemoteLookups` (машина-призрак / марионетка → пир), `ReportRemoteVehicleHit`,
  `ReportRemotePlayerHit`, `IsPvpEnabled`, `IsRemotePlayerNear`.

---

## 11. Визуализация с интерполяцией (`RemotePlayerWorldView`)

- Sink: `spawn`/`move`/`jump`/`action`/`despawn`, отвязан от мира (тестируем).
- **Snapshot-интерполяция** (`RemoteStateTrack.h`, `net::SnapshotBuffer`): каждое
  пересланное состояние несёт серверное время, когда отправитель его снял; view
  держит короткую историю по игроку и показывает его на
  `serverNow − interpolationDelay` (100 мс = два периода потока игрока 20 Гц) — обычно между
  двумя полученными сэмплами: игрок идёт по реальной траектории отправителя
  (включая дугу прыжка) при любом джиттере. Опоздавший пакет покрывается
  экстраполяцией последнего отрезка, не дальше `maxExtrapolationMs` (250 мс). Время
  кадра не влияет.
- `Sync(players)` записывает новые сэмплы (повтор пакета игнорируется) и
  спавнит/деспавнит по составу. `Update` ставит каждого игрока в интерполированную
  позу и ловит фронты (начало прыжка, изменение байта `actionEvent`)
  ОТРИСОВЫВАЕМОГО состояния — прыжок или клип начинается, когда до него доходит
  показываемая поза, а не на задержку интерполяции раньше. Первое показанное
  состояние только «взводит» фронты (без повтора при спавне / появлении).
- Move-sink получает и скорость позы (`RemoteMotion`, по окаймляющим сэмплам). В
  игре (`RemotePlayerMove`) марионетка каждый кадр ставится точно в
  интерполированную позу (после обновления физики мира — последняя запись перед
  рендером), а драйвер локомоции получает ИЗМЕРЕННУЮ горизонтальную скорость и
  направление: цикл шага/бега совпадает с реальным движением, а собственное
  продвижение анимации попадает в следующую позу. Старая схема (локомоция со
  скоростью `speedFactor · maxSpeed` и рывок назад при расхождении в 1 м) и была
  «резинкой».
- `rotY` отправителя — угол взгляда (`Character::GetFacingDir`); раньше туда шёл
  `heading.y` (вертикальная составляющая вектора направления, ~0).
- Непрерывные поля (позиция, `rotY` по кратчайшей дуге, `vehYaw`) смешиваются;
  дискретные (флаги, входы локомоции, состояния) — из ближайшего сэмпла. Поля
  локомоции в игре ведут `LocomotionDriver` марионетки.
- Вход без времени (0: нет часов) показывает новейшее состояние.
- `SetTiming(Timing{clock, delay, extrapolation})`; игра передаёт
  `Session::ServerTimeMs`. View машин-призраков (§6) использует тот же трек.
- `action`-sink получает голый id; удерживаемое действие при спавне не
  проигрывается; `RemotePlayerAction` визуально проигрывает клип.

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

### 12.1 Состояние мира

Канал `WorldEvent` поверх `PID_USER` удалён. Общие объекты мира, экономика и
трафик синхронизируются серверно-авторитетно — см. **§17**. `PID_USER` остаётся
только для модов и пользовательских событий.

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

**Мы делаем движок.** Решения — фундаментальные: общие объекты синхронизируются
через сервер со стабильными детерминированными id; эвристики на приёмнике (поиск
«ближайшего» объекта по позиции и т.п.) недопустимы. Известные ограничения
фиксируются явно (§17.7), а не маскируются.

---

## 17. Реплицируемый мир (серверно-авторитетный)

Все общие игровые объекты синхронизируются **через сервер** со стабильными
детерминированными сетевыми id. Никаких эвристик на приёмнике (поиск «ближайшего
объекта» и т.п. запрещён — см. правило в §16). Модель — **предсказание + арбитраж**:
клиент применяет изменение сразу, сервер решает, итог получают все; отклонённое
предсказание откатывается.

### 17.1 Движковый слой `net::WorldState` (`netengine/WorldState.h`)
- Ключ: `(u64 object, u8 channel)`, значение ≤ `WS_MAX_VALUE` (48) байт, версия,
  владелец, политика, флаги. Лимит `WS_MAX_ENTRIES` (65536) на матч.
- Политики записи (фиксируются создателем записи, понизить нельзя):
  `WSP_LAST_WRITE` (двери, платформы), `WSP_FIRST_WRITE` (сломано/взято — навсегда,
  то же значение не конфликт), `WSP_OWNER` (пишет/стирает только владелец).
  `WSF_ERASE_ON_OWNER_LEAVE` — запись стирается при отключении владельца,
  прочие owner-записи «осиротевают» (owner 0, только чтение).
- Пакеты: `PID_WS_REQUEST` (C→S), `PID_WS_UPDATE`/`PID_WS_ERASE` (S→все, запросивший
  сверяет по seq — подтверждение), `PID_WS_REJECT` (S→запросивший, с текущим
  значением для отката), `PID_WS_SNAPSHOT` (+`_END`) — снапшот новичку.
- Сервер: `WorldStateTable::Apply`. Клиент: `WorldStateMirror` (подтверждённые
  значения + незавершённые предсказания). Sink: `Session::SetWorldStateSink(entry,
  reason, origin)`, причины `WSR_REMOTE/CONFIRM/REJECT/SNAPSHOT/ERASED`.
- **Снапшот для поздно подключившихся:** сразу после WELCOME сервер отдаёт всю
  таблицу чанками ≤ 512 байт и `PID_WS_SNAPSHOT_END` (надёжный упорядоченный
  канал: мир приходит раньше любых последующих изменений).
  `Session::SetWorldSyncedFn` / `WorldStateMirror::IsSynced()`.
- Запросы до WELCOME (и запросы ledger) ставятся в очередь сессии (≤ 256, порядок
  сохраняется) и уходят после WELCOME.

### 17.2 Идентичность объектов (`network/WorldObjectId.h`, `NetWorldRegistry`)
- Статический объект: `id = FNV-1a64(kind, source uid, имя экземпляра uid,
  ordinal)`, старший бит 0. source — файл/сектор, из которого объект загружен;
  ordinal — номер одноимённого экземпляра этого source в порядке загрузки (берётся
  наименьший свободный → перезагрузка сектора даёт те же id). Одинаков на всех
  клиентах, потому что все грузят одни и те же данные.
- Динамическая сущность: `id = 1<<63 | peerId<<32 | счётчик` — выдаётся владельцем
  из server-assigned peer id, коллизий между пирами нет.
- `NetWorldRegistry` (синглтон `NetworkMatchManager::WorldRegistry()`, работает и
  вне матча — объекты регистрируются при загрузке уровня):
  `Register(kind, source, name, handle, apply)` / `Unregister` / `Request` /
  `OnAuthoritative` / `Pump`. Известное сервером состояние объекта, который ещё
  не загружен, применяется при регистрации — отложенно в `Pump()` (никогда внутри
  лоадера/деструктора). Пока идёт применение (`IsApplying`), `Request`
  игнорируется — зеркальные изменения не отправляются обратно.
- Каналы: `WOC_STATE` (состояние stateprop), `WOC_BROKEN` (сломан/уничтожен),
  `WOC_ANIM_DIR` (направление анимации двери/платформы), `WOC_TAKEN` (припаркованную
  машину угнали), `WOC_POPULATION` (план парковки зоны).
- Точки интеграции (`network/NetWorldHooks.h` — лёгкий фасад для игрового кода):
  - `AnimDynaPhysLoader` → `StatePropDSG` (`WOK_STATEPROP`); `SetState` →
    `WOC_STATE`; `eRemoveFromWorld` → `WOC_BROKEN` (запрос до удаления);
    `StatePropDSG::NetApply` применяет; `mNetRemoved` защищает от двойного удаления.
  - `AnimCollLoader` → `AnimCollisionEntityDSG` (`WOK_ANIMCOLL`);
    `SetAnimationDirection` → `WOC_ANIM_DIR`.

### 17.3 Экономика: счётчики (`netengine/Ledger.h`, `network/EconomyConfig.h`)
- Движок даёт серверно-авторитетные **счётчики на игрока** — основа для любых
  экономик (валюта, токены, патроны…). Игра объявляет счётчики и правила по
  причинам изменения (`CounterConfig`): `LR_OPENING` (стартовый баланс, один раз),
  `LR_PICKUP_CLAIMED` (уникальный предмет, нужен claim, один раз на игрока),
  `LR_PICKUP`, `LR_REWARD`, `LR_SPEND`, `LR_LOSS`, `LR_CHEAT`; границы дельты и
  баланса. Неизвестный счётчик/причина — отказ (безопасно по умолчанию).
- `PID_LEDGER_REQUEST`/`PID_LEDGER_RESULT` (результат только запросившему).
  Правила одинаковы у хоста и выделенного сервера (`economy::MakeCoinConfig`).
- **Монеты — у каждого игрока свои**, но с проверкой сервером. Все изменения
  банка идут через `CoinManager::AdjustBankValue(delta, reason, claim)` (и покупки
  в `RewardsManager`) с фактической применённой дельтой. Мировая монета несёт
  claim = детерминированный id (сектор + позиция локатора) — дважды одну монету
  не зачислить. `PlayerEconomy` объявляет стартовый баланс при входе и, когда своих
  запросов в полёте нет, сводит локальный банк к серверному (откат отказов и
  исправление дрейфа). Если сервер не принял стартовый баланс — синхронизация
  экономики для игрока выключается (реальный баланс не обнуляется).

### 17.4 Динамические сущности и трафик (`network/NetEntityManager`)
- Существование, id и владение — на сервере: запись `WS_CHANNEL_ENTITY` (0xFF),
  `WSP_OWNER` + `WSF_ERASE_ON_OWNER_LEAVE`, значение — описание спавна (вид, модель,
  поза). Симулирует владелец и стримит движение `PID_ENTITY_STATE` (пачками);
  сервер пересылает **только записи сущностей, которыми отправитель владеет**.
  Остальные показывают сглаженного кинематического призрака. Новичок получает все
  живые сущности в снапшоте; уход владельца стирает его сущности у всех.
- Трафик: `TrafficManager::AddTraffic` → `AddLocalEntity`, `RemoveTrafficVehicle` →
  `RemoveLocalEntity`. Призраки — `VehicleCentral::AddNetworkGhostVehicle`, общая
  кинематика `ApplyGhostKinematics` (поза, скорость, руль, огни, взрыв по флагу).
  **Общий бюджет плотности:** при спавне учитываются чужие машины рядом
  (`CountRemoteEntitiesNear`), поэтому два игрока рядом не удваивают трафик.
- Уничтожение машины игрока — `VEH_FLAG_DESTROYED` в `PlayerNetworkState`.
- Отрисовка призраков, миграция, удары, урон, interest management: §18.4–§18.6.

### 17.5 Припаркованные машины (`ParkedCarManager`)
- Расстановка зоны (какой тип на каком локаторе, цвет) — first-write запись
  `WOC_POPULATION` объекта `(WOK_PARKED_CAR, зона)`. Первый загрузивший зону клиент
  решает и публикует; остальные и новички используют её; проигравший гонку клиент
  переставляет машины по авторитетному плану (локаторы зоны кэшируются в `NetZone`).
- Каждая поставленная машина — объект `(зона, локатор+1)`: `WOC_TAKEN` при посадке
  локального игрока (у остальных копия убирается — машину дальше зеркалит поток
  игрока), `WOC_BROKEN` при взрыве (у остальных — тот же husk-свап).

### 17.6 Допуск и авторизация
- Авторизация **опциональна** и по умолчанию выключена: без пароля сессия открыта
  (кооператив/сессионки). Пароль проверяется в `PID_HELLO` (§4).
- До допуска сервер принимает от пира только `PID_HELLO` и ничего не сообщает ему
  о мире.
- Лимит слотов: `ServerCore::SetMaxPeers` (`Session::SetMaxPeers`,
  `NetworkMatchManager::SetHostConfig`, `har_server --max`); loopback игры-хоста
  занимает слот, выделенный сервер — нет.

### 17.7 Известные ограничения (открытые задачи)
- Состояние мира, изменённое хостом до старта матча (из сохранения), серверу не
  известно.
- Освобождённую машину трафика можно принять, только если у претендента есть
  свободная машина пула той же модели и дорога под ней; иначе запись истекает по
  окну claim.
- Сдвинутые припаркованные машины, пешеходы и актёры: §18.9.

---

## 18. Транспорт, серверное ядро и синхронизация физики (2026-10-25)

### 18.1 Один сервер для хоста и выделенного
`net::ServerCore` (§4) — единственная реализация сервера. `Session` = клиент +
необязательное встроенное ядро; `har_server` = ядро без клиента. `GameServer`
(игровые правила) подключается к любому. `Session::Create/Get/Destroy` (второй,
неиспользуемый способ владеть сессией) удалены.

### 18.2 Каналы, рукопожатие, часы
§2, §4, §5: три канала ENet с доставкой по id; `PID_HELLO` с magic, версией
протокола и игры; явные причины `PID_REFUSE`; таймаут рукопожатия; серверные часы
в WELCOME/PONG, `ClockSync` на клиенте; каждый поток помечен серверным временем;
snapshot-интерполяция у приёмников (§11).

### 18.3 Валидация движения (`PlayerMovementValidator`)
На сервере, по собственному времени приёма (клиент не может подделать dt).
Относительно последнего ПРИНЯТОГО состояния пира: горизонталь ≤ maxSpeed·dt +
запас (пешком 20 м/с, в машине 110 м/с, если хоть одно из состояний в машине,
запас 4 м); подъём ≤ maxSpeed·dt + запас, падение ≤ 80 м/с·dt + запас;
бесконечные / вне мира координаты отклоняются. Скачок сверх лимитов проходит
только с новым `teleportSeq`, не больше 3 подряд и 0,5/с в среднем. На отказ —
`SV_CORRECT` с последним принятым состоянием (не чаще раза в секунду; состояния
между ними отбрасываются); игра ставит аватара (или его машину) обратно
(`GameplayStateCorrection`). `on_validate` модов выполняется последним.

### 18.4 Лимиты частоты
§4. Действуют на каждого допущенного пира, включая loopback хоста.

### 18.5 Миграция владения
`WorldState`: `WSO_RELEASE` (владелец → 0, флаг `WSF_CLAIMABLE`, при желании с
новым значением = текущая поза) и `WSO_CLAIM` (первый claim побеждает,
последующие отклоняются с указанием победителя). `WSF_RELEASE_ON_OWNER_LEAVE`
освобождает запись, а не стирает, при отключении владельца. Невостребованные
записи истекают через `SetClaimWindowMs` (3 с). `NetEntityManager`: когда
локальная симуляция бросает мигрируемую сущность (трафик, пешеходы), а другой
игрок в радиусе передачи (150 м), она освобождается в текущей позе; игрок в
радиусе claim (120 м) и с запасом бюджета (`CanAdoptFn`) забирает её; при успехе
sink `adopt` превращает призрака в локально симулируемую сущность с тем же id
(`TrafficManager::AdoptNetworkTraffic`: свободная машина пула той же модели на
ближайшей полосе, в той же позе и с той же скоростью, без объявления новой
сущности). Трафик объявляется с `WSF_RELEASE_ON_OWNER_LEAVE`.

### 18.6 Удары и урон
- Призрак (`Vehicle::mNetworkGhost`, ставит `AddNetworkGhostVehicle`) никогда не
  получает локальный урон. В `Vehicle::PostReactToCollision` столкновение НАШЕЙ
  машины с призраком превращается в `NetEntityHit` {импульс на призрака (от нашей
  машины), точка контакта, урон = нормированная сила · 255} →
  `networld::ReportGhostHit`: владельцу сущности (`PID_ENTITY_EVENT`, сервер
  доставляет только владельцу) или, если это машина удалённого игрока, этому
  игроку (PvP, §18.7). Столкновения призраков между собой — дело владельцев.
- Владелец применяет: `Vehicle::ApplyNetworkHit(impulse, damage)` (Δv = импульс /
  масса + `TriggerDamage` + визуальный урон своего типа); результат уходит потоком
  обратно.
- Урон реплицируется: `NetEntityMotion::damage` и `PlayerNetworkState::vehDamage`;
  призраки показывают его через `ApplyNetworkDamage` (урон только растёт).
- `NetEntityMotion` (17 байт с протокола 3, §18.11) получил `damage` и `state`; во флагах — `MF_KNOCKED`.
- Interest management: призрак динамической сущности существует только в радиусе
  релевантности (200 м в игре, удаляется дальше 1,25×; запись сохраняется);
  призрак, которого мир сейчас не может создать, пробуется каждый кадр.

### 18.7 PvP как опция сессии
`SO_PVP` в опциях сессии (WELCOME / `PID_SESSION_INFO`, меняется на ходу:
`GameServer::SetPvp`, `NetworkMatchManager::SetHostPvp`, `har_server --pvp`).
Сервер пересылает `PID_PLAYER_EVENT` жертве только при включённом PvP. Виды
(`NetworkMatchManager::PlayerEventKind`, payload `NetEntityHit`):
`PE_VEHICLE_HIT` (наша машина ударила машину-призрака жертвы → машина жертвы
получает `ApplyNetworkHit`) и `PE_KNOCKDOWN` (наш пинок попал в марионетку жертвы →
`Character::ApplyNetworkKnockdown`: flail + подъём через `InSim`, без скорости —
как оригинал избегает толкать персонажа игрока). Марионетку удалённого игрока
локально не сбивают никогда — с PvP или без (правду показывает поток владельца).

### 18.8 Поставка серверного продукта
`HarServer.vcxproj` (Debug/Release x64) после сборки копирует `lua.dll`;
`src/server/x64/` игнорируется git.

### 18.9 Контекст и внешность игрока (`PlayerPresence`)
Медленно меняющиеся факты об игроке — записи мира собственного объекта игрока
(`MakePlayerObjectId(peer)` = динамический id пира со счётчиком 0; `WSP_OWNER` +
`WSF_ERASE_ON_OWNER_LEAVE`), а не часть потока 20 Гц: надёжно, в снапшоте для
новичков, публикуются только при изменении:
- `WOC_PLAYER_CONTEXT` (6): `u8 level | u64 zone` (uid интерьера, 0 = снаружи).
  `RemotePlayerWorldView::SetPresence` / `RemoteVehicleWorldView::SetVisibility`
  показывают игрока (и его машину) только в нашем контексте; вне его состояние
  отслеживается, но марионетки нет. Неизвестный контекст = показывать.
- `WOC_APPEARANCE` (7): `char model[32] | char anim[16]` — реальная модель (со
  скином) и банк анимаций локального персонажа
  (`CharacterManager::GetModelName/GetAnimName`). Игра снимает их каждый кадр
  (`PublishLocalPresence`), поэтому ЛЮБАЯ смена облика (покупка одежды, миссия,
  чит, будущая система нарядов) реплицируется без дополнительных хуков.
  Марионетка создаётся с этой моделью и банком (вместо фиксированных `homer`/`npd`
  — теперь доступны клипы самого игрока) и меняет облик на месте при изменении
  (sink `restyle` → `CharacterManager::SwapData`). Пока запись не пришла,
  марионетка — `homer`, и обновляется, когда запись придёт.
- `WOC_VEHICLE` (8): `i32 netVehicleId | char model[16]` — машина, которой
  управляет игрок (§18.11).

### 18.11 Трафик: измерение, фильтрация по интересу, компактные потоки (протокол 3)
Замер нагрузочным тестом ниже: 16 игроков в 4 группах по 1 км друг от друга,
двое в интерьере, у каждого 5 машин трафика, поток игрока 20 Гц:

| | исходящий сервера (провод ENet) | состояние игрока | получает игрок |
|---|---|---|---|
| всё всем (`--no-interest`) | 4,4 Мбит/с | 4740 пак/с | 297 состояний/с |
| фильтрация по интересу | 0,9 Мбит/с | 1450 пак/с | 6 / 95 / 110 (интерьер / группа из 3 / из 4) |

Пересылаемый сэмпл игрока — 31 байт с рамкой (4 рамка + 6 заголовок ретрансляции + 21).
- **Учёт.** `ServerEndpoint::GetTraffic()` (`net::TrafficStats`): пакеты/байты с
  рамкой по каждому id, отправленные (по разу на получателя) и принятые, плюс
  итоги провода ENet (полезная нагрузка UDP с заголовками/ack/повторами ENet,
  переносятся в 64-битные счётчики каждый `Update`). `GetPeerLink(peer)`: RTT,
  троттл ненадёжных пакетов (0..32) и потери. `ServerCore::GetTraffic()`,
  `GetPeerLink()`, `Stats::samplesFiltered` / `entityRecordsFiltered`.
- **Фильтрация по интересу** (`net::IInterestPolicy`,
  `ServerCore::SetInterestPolicy`; без политики всё идёт всем). Ядро не читает
  игровые данные, поэтому решает игра, а ядро исполняет: `OnPlayerState` (каждое
  принятое состояние, после валидатора), `PlayerStateIntervalMs(receiver,
  sender)` (0 = каждый сэмпл, `kNotRelevant` = ничего, иначе минимальный
  интервал — ядро хранит у получателя время последнего пересланного сэмпла
  каждого отправителя, допуск — десятая часть интервала), `EntityRelevant(
  receiver, owner, rec)` (каждый получатель получает своё подмножество пачки
  владельца), `OnPeerLeft`. Игровая `PlayerInterestPolicy` (ставит `GameServer`,
  настройки `GameServer::Options::interest`): другой контекст
  (`WOC_PLAYER_CONTEXT`) → ничего; в `nearRadius` 150 м каждый сэмпл; дальше —
  один раз в 200 мс (5 Гц), гистерезис 25 м; запись сущности — только в контексте
  владельца и в `entityRadius` 300 м от получателя (клиент скрывает призраков
  дальше 250 м). Неизвестное (нет записи контекста / ещё нет принятого состояния)
  считается релевантным.
- **Приёмник.** `RemoteStateTrack::Timing::adaptive` (в игре включено): задержка —
  два измеренных интервала между сэмплами, не меньше `interpolationDelayMs`,
  меняется со скоростью 0,25 мс на мс (далёкий игрок на 5 Гц показывается с
  отставанием 400 мс, плавно; смена яруса чуть ускоряет/замедляет воспроизведение
  вместо скачка). Сэмпл более чем через `kGapResetMs` (1 с) после последнего
  начинает новую историю (нет «скольжения» через время, когда игрок был
  нерелевантен). `NetEntityManager`: с часами и радиусом релевантности движущаяся
  сущность показывается (и считается в `CountRemoteNear`) только пока её
  последний сэмпл моложе `kStaleMs` (1 с) — сервер перестаёт её слать, когда она
  далеко; отпущенная сущность берёт итоговую позу из записи отпускания.
- **Компактные потоки.** `PlayerNetworkState` (§3: 21/28 байт вместо 60) и
  `NetEntityMotion` (17 байт вместо 22: `i24 x,y,z | u16 yaw | u16 speedKmh |
  i8 steer | u8 flags | u8 damage | u8 state`). Модель машины ушла из потока:
  запись присутствия `WOC_VEHICLE` (8) `i32 netVehicleId | char model[16]`,
  публикуется наблюдателем локального состояния моста при изменении и
  возвращается в каждое принятое состояние декоратором моста
  (`NetworkMatchManager`), если id совпадает. `PROTOCOL_VERSION = 3`.
- **Один пакет на всех получателей.** `ServerEndpoint::SendToMany` создаёт один
  пакет ENet и ставит его в очередь каждому получателю (ENet считает ссылки);
  через него идут `SendTo`, `Broadcast` и все рассылки `ServerCore`.
- **Троттл ENet.** ENet выбрасывает долю ненадёжных пакетов, когда сэмпл RTT
  больше предыдущего плюс две дисперсии. На тихой линии с RTT 1-2 мс это просто
  дрожание: в нагрузочном тесте до 7/8 потока к 11-13 пирам терялось при 0 %
  потерь на проводе (то же касается реальных игр). `net::ConfigureLink` (обе
  стороны, при соединении) настраивает троттл так, что он никогда не снижается;
  частоты потоков ограничивает сам движок (каденции, интерес, бюджеты пиров).
- **Нагрузочный тест / диагностика.** `har_server --stats <s>` раз в N секунд
  печатает итоги (с рамкой и провод, в обе стороны), сколько сэмплов / записей
  отсёк интерес, худшую линию (RTT, затроттленные пиры, потери) и каждый
  отправленный id (пак/с, кбит/с, байт/пак); `--no-interest` выключает политику
  для сравнения. `mp_standalone bots <count> <ip> [port] [--seconds] [--groups]
  [--spread] [--interiors] [--entities]`: столько полноценных клиентов движка в
  одном процессе (у каждого свой Session / пир ENet), кружат со скоростью 5 м/с,
  с записями контекста и своими потоковыми сущностями; печатает, сколько
  состояний в секунду получил каждый бот.
- **Запуск в фоне.** Игра, запущенная без фокуса окна, минутами стояла в
  `InputManager::Init`: `radControllerDirectInput` повторял `Acquire()` до 2,4 с
  на каждое устройство ввода (`DIERR_OTHERAPPHASPRIO`). Теперь одна попытка —
  опрос всё равно переподхватывает устройство. Две игры + сервер: обе в
  геймплее за 6-14 с.

### 18.12 Посадка в припаркованную машину: претензия и откат (P5)
`network/ObjectClaim.h`: исключительная претензия — запись, значение которой —
peer id претендента (`EncodeClaim`, u16 LE), пишется с `WSP_FIRST_WRITE`; сервер
оставляет первую, каждый клиент узнаёт победителя из авторитетной записи
(проигравший — через `WSR_REJECT`, где лежит текущая запись);
`ClaimOutcomeOf(value, self)` → `CLAIM_OURS` / `CLAIM_THEIRS` / `CLAIM_NONE`.
`self` даёт `networld::LocalPeerId()`.
`ParkedCarManager` (`WOC_TAKEN` припаркованной машины, раньше константа 1 — два
одновременно севших оба «выигрывали»):
- `EVENT_GETINTOVEHICLE_START` локального игрока в сетевую припаркованную машину:
  запоминается текущая машина (AddRef);
- `EVENT_GETINTOVEHICLE_END`: отправляется претензия (посадка предсказана);
- вердикт `CLAIM_OURS`: посадка остаётся; `CLAIM_THEIRS`: если игрок в машине (или
  садится), запускается штатная высадка игры (`GetOut`, на ходу — выпрыгивание), и
  по `EVENT_GETOUTOFVEHICLE_END` возвращается прежняя текущая машина, а наша
  копия машины удаляется — машину победителя показывает его поток игрока; иначе
  наша копия просто удаляется.
- Менеджер был подписан на `EVENT_GETINTOVEHICLE_END` дважды (претензия уходила
  два раза); теперь — по разу на START / END / GETOUT_END.

### 18.13 Толкание припаркованных машин (P3)
Припаркованная машина — статический объект мира (id реестра, `WOK_PARKED_CAR`);
пока её толкают, она ещё и сущность, ПРИВЯЗАННАЯ к этому id (`NEK_PARKED_CAR`,
`AddLocal(handle, spawn, boundId)` — запись сущности лежит под id самой машины с
`WSP_OWNER`, поэтому первый толкнувший — владелец, второй получает `WSR_REJECT` и
следует за ним).
- Кто двигает: `Vehicle::PostReactToCollision` — контакт машина-машина
  (нормированный импульс > 0,01) с машиной, симулируемой ЗДЕСЬ (не призрак: наша
  машина, наш трафик), вызывает `ParkedCarManager::OnPushedLocally`, который
  объявляет привязанную сущность (`networld::ObjectIdOf`). Толчок призрака — дело
  его владельца.
- Владелец: сэмплер шлёт её как трафик; `UpdateNetworkPushes` (каждый кадр, из
  `GameplayContext::OnUpdate`) останавливает поток, когда машина стоит (< 1 км/ч)
  1 с: поза покоя пишется как `WOC_POSE` (6, `net::ObjectPose`, f32 x,y,z,yaw,
  `WSP_LAST_WRITE`), сущность стирается; посадка в неё тоже останавливает поток
  (дальше — поток игрока и `WOC_TAKEN`).
- Остальные: spawn сущности для `NEK_PARKED_CAR` —
  `ParkedCarManager::NetDriveBound(boundId)`: наша копия
  (`networld::ObjectHandleOf`) переходит в `Vehicle::SetNetworkDriven(true)`
  (кинематика `simAICtrl`, как у трафика, плюс `mNetworkGhost` — наши удары по ней
  уходят владельцу) и идёт за потоком; despawn — `NetReleaseBound`, обратно в
  нашу физику. Если зона машины здесь не загружена, spawn повторяется, пока
  сущность жива.
- `WOC_POSE` применяет `NetApplyCar` (`Vehicle::SetTransform`), если машину сейчас
  не двигают и не ведут, — в том числе при регистрации, так что новичок или
  перезагруженная зона ставит машину туда, где её оставили, а не на локатор.
- `RemoveParkedCar` сначала останавливает наш поток и снимает сетевое ведение.

### 18.10 Ещё открыто
- Пешеходы / актёры как сущности (`NEK_PEDESTRIAN`/`NEK_ACTOR` зарезервированы).
- Нет идентичности игрока между переподключениями (T7).
