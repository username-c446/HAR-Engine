# План: точный мультиплеер (анимации, машины, мелкие нюансы)

Статус: план (2026-10-24). Основан на аудите кода, все ссылки `file:line`.
Документ дополняет `MULTIPLAYER_SYNC_MANIFEST.ru.md` (что синхронизировать) —
здесь конкретика *как*, с полями, транспортом и тестами.

---

## 0. Текущее состояние (что уже синхронизируется)

| Что | Где | Статус |
|---|---|---|
| Позиция x,y,z | `PlayerNetworkState.h:7`, `gameplaycontext.cpp:153` | ✅ отправляется и сглаживается |
| rotY | `PlayerNetworkState.h:8`, `gameplaycontext.cpp:159` | ⚠️ **отправляется, но НИГДЕ не потребляется** (ни пеший, ни машина) |
| В машине ли + модель | `vehicleModel[16]`, `netVehicleId` | ✅ зеркалится в `RemoteVehicleWorldView` |
| Fleet-скорость машины | `vehicleSpeed` | ⚠️ только для plausibility-проверки сервера, не для зеркала |
| Прыжок/в-воздухе | флаги `FLAG_JUMPING/IN_AIR` | ✅ rising-edge триггерит jump-клип локально |

Канал: 42 байта / снапшот / ~10 Гц (в 6-й кадр при 60 fps, `Session.h:162`),
запас пакета 512 (`Packet.h:18`) → blob можно расти почти в 10 раз без риска.

**Главные дыры:** анимации не передаются вообще; `rotY` не применяется;
машинное зеркало — голый `SetPosition` (`gameplaycontext.cpp:355`), без
поворота/скорости/руля/ушерба; миссия-прогресс не проброшен в Session
(no-op провайдер). Ниже — как закрыть всё.

---

## 1. Анимация пешего персонажа (приоритет — высший)

### 1.1 Непрерывный локомоушн (работает всегда)
Восстанавливать позу удалённого игрока локальным движком пуппета —
переиспользовать `Puppet::PlayLocomotion(name/uid, motionAngle, facingAngle,
velocity)` (`puppet.hpp:212-218`). Это blend-tree: отдал 3 числа → получил
правильный walk/run/idle.

**Добавить в `PlayerNetworkState`:**
- `motionAngle` — uint16 (рад, кадраж 0.01), источник `GetDesiredMotionAngle`/`mfDesiredDir`;
- `facingAngle` — uint16, источник `SetFacingDir`/`GetFacingDir`/пуппет-фейсинг;
- `speed` — uint8 (0..макс, кадраж), источник `GetDesiredSpeed()`/`mfDesiredSpeed`;
- `characterState` — uint8, enum `CharacterState` (`state.h:32-40`): gating, LOCO→locomotion;
- `turbo` — 1 бит (`IsTurbo()`), влияет на clamp максскорости.

Итого ~6-7 байт к 42. Приёмник в `LOCO` вызывает `PlayLocomotion`, пуппет сам
доворачивает (facing-сглаживание в `LocomotionDriver::UpdateFacing`).

### 1.2 Одноразовые анимации (rising-edge события)
НЕ слать позу — слать id действия один раз, клип докатывается локально.
Клипы фиксированы/модель-независимы (`action.cpp`), кроме idle (`%s_idle%d`,
модель-зависим). Латчить по rising-edge.

Добавить `actionEvent` uint8 в `PlayerNetworkState` (или через `WorldEvent`):

| id | клип/поведение | источник |
|---|---|---|
| JUMP | `jump_idle_launch`→`jump_idle`/`jump_dash_in_air` | `state.cpp:733`, уже есть `FLAG_JUMPING` |
| SLAM | `stomp_in_air`→`stomp_land` | `JumpAction` `action.cpp:1922` |
| JUMPKICK | `jump_kick` | `action.cpp:2135` |
| DODGE | `dodge` | `DodgeAction` `:2188`, `state.cpp:691` |
| CRINGE | `cringe` | `CringeAction` `:2257` |
| FLAIL | `flail` (сбит/падение) | `FlailAction` `:2311`, `InSim::Enter` `:1840` |
| GETUP | `get_up` | `GetUpAction` `:2380` |
| KD | переход в `INSIM` (#физик-сбит) | `character.cpp:2581` |
| ATTACK | `break_quick` | `state.cpp:807` |
| CELEB_SMALL/BIG | `victory_small`/`_large` | `state.cpp:819/832` |
| SURF | `surf_in`/`surf_out` | `action.cpp:391` |
| OFFHAND_ENTER/EXIT | get-in/get-out path | `state.cpp:198` |

Механизм уже задел: `RemotePlayerWorldView` имеет rising-edge `jump`-sink
(`RemotePlayerWorldView.h:23`). Расширить до общего «анимационного канала».

**Сглаживание по прыжку `JumpFlagSnapsVerticalHeightInstantly` (`network_tests.cpp:923`)
остаётся эталоном**: вертикаль-снап, горизонт-сглажено. Для dive/flail — та же
модель (Y снап, XZ сглажено), плюс локальный `flail` на `KD`.

### 1.3 В машине
Живой позу не слать: пуппет приоритетно на `in_car_idle_driver`/`in_car_idle`
(`action.cpp:467`), привязан к heading машины-призрака (который мы научим
поворачивать, см. §2). Нужно:
- `isDriver` 1 бит (клип водителя/пассажира);
- `carBodyVisible` 1 бит (зеркалит `InCar::Enter` `state.cpp:254-261`): если
  машина с `mVisibleCharacters=false` — тело не рендерить.

---

## 2. Точное зеркало машины (приоритет — высокий)

Сейчас `RemoteVehicleMove` = только `SetPosition` (`gameplaycontext.cpp:349-356`),
и `SetTransform` **обнуляет скорость** (`vehicle.cpp:971-974`) — колёса не
крутятся, физика сбрасывает. Это чинить в первую очередь.

**Решение — кинематическое зеркало, а не живущая физика:** призрак не должен
сам считать физику (иначе борется со стримом). Обновлять явно из стрима.

**Набор полей в `PlayerNetworkState` (машинные, когда `inVehicle`):**
| Поле | тип/бит | источник | интерполировать |
|---|---|---|---|
| yaw (facing) | float/uint16 | `GetFacingInRadians()` `vehicle.cpp:1425,1441` | да, по кратчайшей дуге |
| steer angle | uint8 (−45..+45°) | `mWheelTurnAngle` `vehicle.h:310`, применить `SetWheelTurnAngleDirectlyInRadiansForDusitOnly` `vehicle.cpp:1868` | да, легко |
| speed | uint16 (0..300) | `mSpeedKmh` `vehicle.cpp:2313` | да |
| vehicleState | u8 (0-2) | `mVehicleState` `vehicle.h:61` (VS_NORMAL/SLIP/EBRAKE) | — |
| brake / reverse | 1 бит × 2 | `mBrakeLightsOn`/`mReverseLightsOn` `vehicle.h:315-316` | — |
| damage% | u8 | `GetVehicleLifePercentage` `vehicle.cpp:6043` | — |
| destroyed/husk | 1 бит | `mVehicleDestroyed` | событие |

**Ключевые правки на приёмнике:**
1. после позиционирования призрака руками задать `mSpeed`/`mSpeedKmh`
   (отдельно от `SetTransform`, т.к. он их зануляет);
2. yaw применять через матрицу (не только позицию): `m.FillRotateXYZ(0,ang,0); m.FillTranslate(pos); SetTransform` (`vehicle.cpp:1391` паттерн), затем докрутить steer через директиву-сеттер;
3. brake/reverse → `mGeometryVehicle->ShowBrakeLights/HideBrakeLights` (`geometryvehicle.h:84-88`);
4. повреждение — по событию (см. §4), т.к. у призрака `mVehicleCanSustainDamage=false`.

---

## 3. Считать `rotY` на приёмнике (баг сегодня)

`rotY` шлётся, но ни `RemotePlayerWorldView`, ни `RemoteVehicleWorldView` его
не читают (`RemoteVehicleWorldView.cpp:48-50` игнорирует). Это:
- **пеший**: заворачивать пуппет-facing (см. §1.1 `facingAngle`);
- **машина**: yaw зеркала (см. §2).

Без этого нет даже правильного «стоит лицом ко мне» — фундамент для всего
остального. Делать первым, вместе с §1.1.

---

## 4. Разрушение машин / ущерб / остов (через WorldEvent)

Каркас уже есть: `WE_VEHICLE_DESTROYED = 0x03` (`WorldEvent.h:34`), кодек и
релей через `PID_USER` работают (`NetworkMatchManager.cpp:124-134`).

**Не хватает хука-эмиттера.** Разрушение сейчас палит `EVENT_VEHICLE_DESTROYED`
/ `EVENT_VEHICLE_DESTROYED_BY_USER` (`vehicle.cpp:2693,2715`) — там и цеплять
`SendWorldEvent(WE_VEHICLE_DESTROYED, {netVehicleId, x,y,z})`. На приёмнике —
подмена зеркала на остов (свап модели/удаление призрака).

**Ущерб (дробный)** — редкое поле (см. §2, damage%), обновлять при изменении
≥5%: либо пет-механизм в `PlayerNetworkState`, либо новый тип
`WE_VEHICLE_DAMAGE = 0x06` (следующий свободный, тэги заняты до 0x05).

---

## 5. Прочее важное (что дополнительно стоит синхронизировать)

### 5.1 Миссия-прогресс — ДОВЕСТИ ДО СЕССИИ
`SetMissionStateProvider` — no-op (`NetworkMatchManager.cpp:143-149`),
`MissionSyncState` (13 байт) только «пигги-бэком» внутри player-blob
(тест `MissionStateRelaysBetweenClientsThroughServer` `:1395`), отдельного
канала нет (`PID_MISSION_STATE=0x0102` не маплен в Session). Довести:
отдельный `kIdMission`, пробросить провайдера, читать на приёмнике — чтобы
партнёры видели актуальный stage/objective.

### 5.2 Состояние мира / разрушаемость / пропсы
Каркас `WE_STATEPROP/WE_BREAKABLE/WE_COIN_COLLECTED/WE_PLATFORM_DIR` есть,
но **нет игровых эмиттеров** (только тесты). Подключить реальные хуки:
- `StatePropDSG.m_CurrentState`+`mfDirection` (двери/платформы);
- `PersistentWorldManager`-битмаска (уничтоженные объекты);
- монеты (`coinmanager`) — уже частично пер-койн в обходе.

### 5.3 Спавн/деспавн игроков как событие
Сейчас призрак появляется по первому стриму (`RemotePlayerWorldView::SyncOne`).
Для «вошёл/вышел игрок» лучше явный `WE_PLAYER_JOIN/LEAVE` — даёт входной
экран/звук, а не молчаливый допуск удалённого.

### 5.4 Мир-состояние (worldSeed)
Уже рассылается при WELCOME (`Session.cpp:113-118`, `GetWorldSeed`).
Статика сходится. Держать как есть.

### 5.5 Точки входа/выхода из машины
Get-in/get-out — хореографированные пути (`calcGetInPath`, `state.cpp:198`).
Минимум — рассылать `characterState=GET_IN/GET_OUT` + использовать door/seat
события; дверцай закрытие уже поднимается (`GetOut::HandleEvent`).

### 5.6 Тайминги/кадраж
14 кадраж по фикс. 60 fps → ~10 Гц — мало для «резких» анимаций (додж,
cringe). План: непрерывный локомоушн на 10 Гц + сглаживание пуппетом —
норма; одноразовые клипы не зависят от кадража (rising-edge). Для машин yaw/speed
тоже 10 Гц + dead-reckon. Опционально поднять `SetStateSendEvery` до 3-4 для
скильная-машин, но это утроение трафика — после замера.

---

## 6. Полей в итоге (дельты к 42 байтам)

Пеший: `motionAngle u16 + facingAngle u16 + speed u8 + characterState u8 +
actionEvent u8 + turbo 1b + isDriver 1b + carBodyVisible 1b` ≈ **+8 байт**.
Машина: (`yaw u16 + steer u8 + speed u16 + state u8 + brake/reverse 2b +
damage u8`) при `inVehicle` — те же окна, ~+8 байт.
Итого blob ≈ **50-58 байт** = ~12% от капсулы 512. Трафик @10 Гц: ~0.6 KB/s
на игрока вверх, ~0.6·(N−1) вниз.

---

## 7. Порядок работ + тесты (каждый шаг — сборка и зелёный сьют)

1. **rotY → facing** (пеший) + `RemotePlayerWorldView` читает его.
   *Тест:* `NetworkedFacingYawSmoothsShortestArc` / расширить `InterpolationApproachesTargetWithoutTeleporting`.
2. **Непрерывный локомоушн**: добавить 4 поля, `Puppet::PlayLocomotion` на приёмнике.
   *Тест:* `LocoFieldsRoundTrip` + `RemotePoseDrivesLocomotionDriver`.
3. **Одноразовые клипы (actionEvent)**, обобщить jump-sink.
   *Тест:* `ActionEventRisingEdgeFiresOnce`, `DiveFlailReplicates` (по образу `JumpFlagSnaps...`).
4. **Машинное зеркало**: yaw+steer+speed+state+brake; кинематик, не физика.
   *Тест:* `VehicleStateRoundTrip`, `GhostYawApplies`, `SetTransformPreservesReportedSpeed`.
5. **Husk/разрушение**: `WE_VEHICLE_DESTROYED` эмиттер.
   *Тест:* `VehicleDestroyedRelays`, `HuskSwapOnRemote`.
6. **Миссия-канал**: отдельный `kIdMission`, провайдер, приём.
   *Тест:* `MissionStateRelaysThroughDedicatedChannel` (заменить пигги-бэк).
7. **Разрушаемость/пропсы/монеты: реальные эмиттеры** `WorldEvent`.
   *Тест:* `BreakableStateRelaysWithOwner`, `PlatformDirRoundTrip`.

Живая проверка после шагов 2-4: `mp_standalone` + лог remote-позы (расширить
вывод, как сделано для state), два клиента — убедиться, что удалённый реально
«идёт/поворачивает», а машина разворачивается и колеса поворачивают.

---

## 8. Открытые вопросы (решить при реализации)

- Анимационный стейт-машина: слать `CharacterState` (6 знач.) или богаче
  `eIntention` (CharacterController.h:17-45)? Рекомендация: `CharacterState`
  достаточно для gating; `eIntention` — если нужен додж/атака как непрерывность.
- Кинематик-призрак машины vs монтёр физики: выбрано кинематик (чисто и без
  jitter), но поведение при ударе/взрыве надо подстраховать событиями.
- Повышение кадража (SetStateSendEvery 6→3) — после бенчмарка, не как дефолт.