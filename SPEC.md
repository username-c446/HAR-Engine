# MultiTool — «The Simpsons: Hit & Run» (SRR2) — Спецификация мультиплеера

> **Статус:** живой документ. Обновляй и сверяйся с ним при каждой разработке,
> чтобы не создавать дублирующий функционал. Помечай реализованное фичами и
> описывай новые связи функций по мере их появления.

Этот файл описывает весь программно реализованный поверх оригинального SRR2
геймплей и, прежде всего, мультиплеерный стек — как части взаимодействуют между
собой. Он предназначен и для другой нейронки/разработчика, изучающей проект, и
для самого ассистента, чтобы не путаться.

---

## 1. Карта проекта (область сети)

Все сетевые файлы лежат в `src/game/network/`. Основные проекции:

| Файл | Роль |
|------|------|
| `NetworkPacket.h` | Формат пакетов, типы, little-endian сериализация, константы |
| `PlayerNetworkState.{h,cpp}` | Структура состояния игрока (позиция/поворот/машина) + сериализация |
| `NetworkServer.{h,cpp}` | Авторитетный сервер: допуск, сессии, валидация, релей |
| `NetworkClient.{h,cpp}` | Клиент: подключение, heartbeat, приём реестра/состояний |
| `NetworkMatchManager.{h,cpp}` | Оркестратор: host/join/leave, провайдер локального состояния |
| `RemotePlayerManager.{h,cpp}` | Реестр удалённых игроков (идент. по clientId) |
| `RemotePlayerWorldView.{h,cpp}` | Визуализация удалённых пеших игроков + интерполяция |
| `RemoteVehicleWorldView.{h,cpp}` | Визуализация «призраков» машин удалённых водителей |
| `ServerModManager.{h,cpp}` | Серверные Lua-моды (хуки допуска/валидации) |
| `MissionSyncManager.{h,cpp}` | Синхронизация миссии/стадии/цели между клиентами (co-op-прогресс) |
| `MissionSyncState.{h,cpp}` | Сериализуемое состояние миссии (миссия/стадия/цель) |
| `NetworkConsoleCommands.{h,cpp}` | Консольные команды `host`/`join`/`match` |
| `ENetInit.h` | RAII-счётчик ссылок ENet |

Интеграция в игру: `src/game/contexts/gameplay/gameplaycontext.cpp`
(провайдер состояния, sink-и, вызовы в игровом цикле) и
`src/game/main/commandlineoptions.{h,cpp}` (автостарт из CLI).

---

## 2. Сетевая модель и транспорт

- **Транспорт:** ENet (vcpkg `enet`), канал `GAME_CHANNEL = 0`, все игровые
  пакеты надёжные (`ENET_PACKET_FLAG_RELIABLE`). Порядок и доставку гарантирует
  ENet; свой протокол — только тип + payload.
- **Архитектурный принцип:** авторитетный сервер (а не P2P). Сервер допускает
  клиентов, выдаёт `clientId`, валидирует состояния, ретранслирует их, ведёт
  реестр сущностей и раздаёт его клиентам. Клиент не само-авторизуется.
- `MAX_PACKET_SIZE = 512` байт. `CLIENT_TIMEOUT_MS = 8000`, `PING_INTERVAL_MS = 2000`.
- Данные линейны, big-fat-протокол отсутствует: payload может содержать
  повторяющиеся `clientId` (в `PLAYER_STATE`), но полный id — всегда
  авторитетная забота сервера.

### Типы пакетов (`net::PacketType`, 1-байтовый тег в начале)

| Тег | Имя | Направление | Назначение |
|-----|-----|-------------|------------|
| 0 | `HELLO` | C→S | Анонс после ENet-коннекта |
| 1 | `WELCOME` | S→C | Авторитетный `clientId` (аналог адмиссии) |
| 2 | `PLAYER_STATE` | C→S→C | Состояние игрока (релей) |
| 3 | `DISCONNECT` | C→S | Объявление об отключении |
| 4 | `PLAYER_LIST` | S→C | Авторитетный реестр присутствующих сущностей |
| 5 | `VEHICLE_STATE` | зарезервирован | (описан в §6 как часть PLAYER_STATE на практике) |
| 6 | `PING` | C→S | Heartbeat + замер RTT |
| 7 | `PONG` | S→C | Ответ на ping |
| 8 | `AUTH` | C→S | Пароль для защищённого хоста |
| 9 | `MISSION_STATE` | C→S→C | Прогресс миссии/стадии/цели (релей, для co-op) |

> Примечание: `PACKET_VEHICLE_STATE` объявлен в enum, но текущая реализация
> переносит транспортные данные внутри `PLAYER_STATE` (см. §6), т.е. пакет 5
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
    uint8 padding;            // выравнивание размера
};
```

- `SERIALIZED_SIZE = 4*4 + 16 + 4 + 4 + 1 + 1 = 42` байта.
- Сериализация little-endian совместимая, свободна от padding-багов
  (`Serialize`/`Deserialize` в `PlayerNetworkState.cpp` через `net::Write*`).
- **Связь с валидацией:** поле `inVehicle` выбирает лимит скорости на сервере
  (`kMaxSpeedOnFoot ≈ 15 м/с`, `kMaxSpeedInVehicle ≈ 120 м/с`).
- **Связь с машинами:** `vehicleModel` + `netVehicleId` идут в
  `RemoteVehicleWorldView`, чтобы приёмник создал «призрака» машины той же
  модели. `netVehicleId` выводится из `mVehicleCentralIndex` (или хэша указателя
  как запас) — стабилен в рамках сессии.

---

## 4. Сервер `NetworkServer`

### Подключение (допуск)
1. ENet-коннект → назначение `clientId` (`m_nextClientId++`, стартует с 1000).
2. **Lua-мод** может отклонить: `OnPlayerJoin(clientId)` == false → разрыв.
3. Создание `ClientSession` (peer, таймстемпы, `authenticated`).
4. Если пароль включён (`m_requiresAuth`) — ждём `AUTH`; без пароля —
   `WelcomeClient()` сразу.

### Сессии (`ClientSession`)
- `peer`, `lastStateTimeMs`, `connectedAtMs`, `lastSeenMs`, `validated`,
  `authenticated`.
- `lastSeenMs` обновляется на **любой** входящий пакет → таймаут-свип.

### Heartbeat / сброс мёртвых
- `PACKET_PING` → сервер отвечает `PONG` и обновляет `lastSeenMs`.
- В конце `Update()` производится свип: если `now - lastSeenMs > m_clientTimeoutMs`
  → клиент считается мёртвым, вызывается `OnPlayerLeave`, он удаляется из всех
  структур и `enet_peer_disconnect`.
- `SetClientTimeoutMs` переопределяет окно (для тестов).

### Валидация и релей (`ValidateAndRelay`)
- Проверка правдоподобия: телепорты/сверхскорости отсекаются; скорость зависит
  от `inVehicle`. При превышении состояние не ретранслируется (последнее честное
  остаётся в реестре).
- Lua-хук `OnPlayerValidate` может дополнительно ужесточить.
- Принятое состояние кладётся в `m_players` и ретранслируется всем остальным
  клиентам как `PLAYER_STATE` (с sender = `clientId`).

### Реестр сущностей (`SendPlayerListToAll`)
- После каждого подключения/отключения/свипа сервер шлёт всем `PLAYER_LIST`:
  `uint16 count` + массив `clientId`. Это авторитетный источник совстава мира.

### Аутентификация (`PACKET_AUTH`)
- Сервер хранит `m_password[65]` + `m_requiresAuth`.
- Если пароль задан и клиент присылает корректный — помечается `authenticated`
  и приветствуется; иначе `enet_peer_disconnect`.

### Конфигурация
- `SetMaxPlayers(slots)` — лимит одновременно подключённых (только до `Start`).
- `SetPassword(pw)` — включение защиты; `SetClientTimeoutMs`.

---

## 5. Клиент `NetworkClient`

- `Connect(ip, port)` → ENet host, `SetPassword` при наличии.
- После ENet-коннекта шлёт `HELLO` и, если был задан пароль, `AUTH`.
- При получении `WELCOME` запоминает `m_clientId` и переходит в
  `m_serverReady = true`, сбрасывая `remotePlayers`/авторитетный список.
- `PLAYER_LIST` → перестроение набора удалённых, **diff** против прошлого
  авторитетного списка генерирует события join/leave (см. §7).
- `PLAYER_STATE` от других клиентов → `UpdatePlayer` в `m_remotePlayers`
  (свой собственный эхо игнорируется по `clientId`).
- Heartbeat: каждые `m_pingIntervalMs` шлёт `PING`, фиксирует `m_pingSentMs`;
  по `PONG` считает RTT → `GetPingMs()` / `HasMeasuredPing()`.
- `SendPlayerState` — rate-limited (`kStateEveryFrames = 6`, ~10 Гц).

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

### Sink в gameplay-контексте (`RemoteVehicleSpawn/Move/Despawn`)
- Спавн через `VehicleCentral::InitVehicle(model, addToActiveList, ...,
  VT_TRAFFIC, FORCE_NO_DRIVER, ...)` — без водителя, транспорт создаётся как
  обычный активный.
- Move — `SetPosition`; Despawn — `RemoveVehicleFromActiveList` + `delete`.

---

## 7. Игроки: список и уведомления (C8/C9)

- `NetworkClient` при обработке `PLAYER_LIST` делает diff между
  `m_prevAuthoritativePlayers` и новым списком:
  - id был, теперь нет → событие leave;
  - id нет, теперь есть → событие join.
- События скапливаются в `m_pendingEvents` (`JoinLeaveEvent{clientId, joined}`)
  и забираются через `DrainJoinLeaveEvents` (через `NetworkMatchManager`).
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

- `--password` → `server.SetPassword()` / `client.SetPassword()`.
- `--max` → `server.SetMaxPlayers()`.
- `--log` → файл лога (строка запуска + `clients:N` при смене состава).
- `--mod` → `server.LoadServerMod()`.

### CLI основной игры (`CommandLineOptions`)
- `HOSTPORT=<port>` — автостарт хоста.
- `JOINIP=<ip[:port]>` — автостарт join.
- `HOSTPASS=<pw>`, `HOSTSLOTS=<n>`, `JOINPASS=<pw>` — конфигурация.
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

Единственный сетевой вход из геймплея. Один процесс = **либо хост, либо
join-клиент, никогда оба** (`HostAndJoinAreMutuallyExclusive`).

- `StartHost(port)` / `JoinHost(ip, port)` / `Leave()` — жизненный цикл матча.
- `SetHostConfig(password, maxPlayers)`, `SetClientPassword(pw)` — конфиг.
- `SetLocalStateProvider(fn)` — провайдер локального состояния.
- `GetRemotePlayers()` — реестр (из сервера или клиента).
- `DrainJoinLeaveEvents(out)` — уведомления join/leave.
- `Update()` — крутит сервер/клиент; на клиенте по таймеру шлёт локальное
  состояние через провайдера.

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

---

## 12. Синхронизация миссии (A3, co-op)

Миссии становятся **совместными только когда игроки проходят их вместе**, а не
принудительно для всех. Механика — релей прогресса миссии, как у `PLAYER_STATE`:

- **Состояние:** `MissionSyncState` (миссия/стадия/цель + флаг `inMission`),
  сериализация как у `PlayerNetworkState`.
- **Локально:** `GameplayMissionStateProvider` в `gameplaycontext.cpp` читает
  текущую миссию через `GameplayManager::GetCurrentMission()` /
  `GetCurrentStageIndex()` и шлёт по сети (клиент → сервер → остальные).
- **Сервер:** принимает `PACKET_MISSION_STATE`, хранит по клиенту в
  `m_missionStates`, ретранслирует остальным (без gate-валидации — ход миссии
  решает хост).
- **Клиент:** `GetRemoteMissionStates()` отдаёт `map<clientId, MissionSyncState>`
  для UI «кто в какой миссии».
- `MissionSyncManager` агрегирует локальный + удалённые прогрессы,
  тестируем.

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
- Прогон: `vstest.console.exe` по `Tests.dll`. Текущее состояние — 30/30 зелёных.
- Сборка полной игры: `MSBuild.exe src/srr2/SRR2.vcxproj
  /p:Configuration=DebugWindows /p:Platform=x64
  /p:WindowsTargetPlatformVersion=10.0.22621.0`.

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
- **MP-стабильность** — проверить вход host/join в игре end-to-end (персонаж
  управляется, клавиша взаимодействия работает, выход без ассертов), сетевой
  матч между двумя копиями.

---

## 16. Правило на будущее

**Перед любой разработкой и в ходе — сверяйся с этим документом.** Если фича
уже описана, не создавай её заново и не добавляй дублирующий транспорт/пакет
(пример: `PACKET_VEHICLE_STATE` объявлен, но данные машин живут в
`PLAYER_STATE` — не плоди второй путь). После крупных изменений обновляй этот
файл.