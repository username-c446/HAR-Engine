# Разработка кастомного HUD/оверлея в UI-слое движка

> **Статус:** документация по опыту команды/ассистента — как добавлять и
> редактировать элементы интерфейса (overlay/HUD) в движке, где обычно ломается,
> и как это чинить. RU-версия; английская — `HUD_AND_OVERLAY_DEV.md`.
>
> См. также `SPEC.ru.md` (секции §14–16) и `README.ru.md`.

Эта статья — практическое руководство по работе с пользовательским
интерфейсом движка (меню, HUD, оверлеи). Она родилась из реального опыта
добавления пункта «MULTIPLAYER» и оверлея ввода IP в главное меню, и призвана
сэкономить время тому, кто захочет писать свои экраны или HUD-элементы.

---

## 1. Как устроен UI в движке (кратко)

Вся графика интерфейса (меню, HUD, заставки) — это **Scrooby**-проекты, которые
компилируются в файлы **Pure3D** (`.p3d`). Иерархия:

```
FrontendProject (0x18000)
 └─ Screen (0x18001)
     └─ Page (0x18002)
         └─ Layer (0x18003)
             ├─ Group
             ├─ Sprite
             ├─ Text
             └─ MultiText
```

Ключевые факты о `.p3d`:

- Формат **полностью расшифрован** в наших исходниках парсера:
  `src/libs/scrooby/src/FeProjectChunkHandler.cpp` (`LoadScreenChunk`,
  `LoadPageChunk`, `LoadLayerChunk`, `LoadTextChunk`, `LoadSpriteChunk`,
  `LoadGroupChunk`), плюс `FeProjectLoader.cpp`, `FeScreen.cpp`, `FePage.cpp`,
  `FeLayer.h`. Для чтения/патчинга ассетов этого достаточно.
- **В Scrooby-рантайме НЕТ сериализатора/писателя `.p3d`** — только чтение
  (`tChunkFile`). Создавать свой `.p3d` можно либо внешним
  **Lucas' Pure3D Editor**, либо собственным чанк-писателем по образцу `Load*`.
- Внешняя документация
  [machinesinmotion/Pure3DFiles](https://github.com/machinesinmotion/Pure3DFiles)
  (donutteam/docs) неполна (без заголовка чанка, endianness) и уступает нашим
  исходникам.

---

## 2. Подход «кодом без новых ассетов» (рекомендуется)

Для новых элементов (оверлей мультиплеера, хинты, кастомный HUD) **не требуется
генерировать новый `frontend.p3d`**. Всё, что нужно, — взять уже существующий в
ассете скрытый/служебный элемент и переиспользовать его из кода:

- Ищем на нужной странице **скрытый `Text`**, который не является пунктом меню
  (например, `MainMenu` vs `MainMenu_PC` в главном меню на Windows).
- Подключаем его из `CGuiScreen*`:
  ```cpp
  Scrooby::Text* t = dynamic_cast<Scrooby::Text*>( page->GetText( "MainMenu" ) );
  ```
- Настраиваем геометрию/выравнивание из кода:
  ```cpp
  t->SetVisible( true );
  t->SetIndex( 0 );                       // нужную строку MultiText
  t->SetTextMode( Scrooby::TEXT_OVERLAP );// не переносить по пробелам
  t->SetBoundingBoxSize( 800, 120 );
  t->SetHorizontalJustification( Scrooby::Centre );
  t->SetVerticalJustification( Scrooby::Centre );
  t->SetPositionOfCenter( 400, 240 );
  // ... потом:
  t->SetString( 0, "MULTIPLAYER HOST\nAddress: ..." );
  ```

Так мы получаем полноценный читаемый промпт поверх меню **без единого нового
ассета** и без нарушения навигации.

### Почему именно скрытый/отдельный текст

- Пункты меню — это **MultiText со строками по индексу выбора**, и меню
  перезаписывает их (`SetString`, пульсация цвета/размера каждый кадр).
- Если писать оверлей **в пункт меню**, то:
  - длинная строка ломает layout пункта;
  - `UpdateCurrentSelection` пульсирует размер `ResetTransformation() +
    ScaleAboutPoint()` — текст «прыгает»;
  - можно случайно сломать навигацию (меню циклирует выбор, клавиши
    «перескакивают»).
- Поэтому пишем в отдельный от меню текст.

---

## 3. Поле ввода текста в оверлее (`TextInputHook`)

Игра по умолчанию **не передаёт** набираемый с клавиатуры текст в GUI-экраны
(ввод ограничен геймпадом). Для ввода IP/порта в оверлее сделан крошечный мост:

- Файлы: `src/game/presentation/gui/utility/textinputhook.{h,cpp}`.
- Идея: глобальный хук с **одним активным потребителем**:
  ```cpp
  TextInputHook::SetActive( OnMpOverlayChar, OnMpOverlayBackspace );
  TextInputHook::ClearActive();
  TextInputHook::HasActiveConsumer();
  ```
- Платформенная часть (`win32platform.cpp`) в SDL-цикле:
  - `SDL_StartTextInput()` при инициализации окна — **без этого SDL вообще не
    шлёт `SDL_TEXTINPUT`** (типичная ошибка — «ввод не работает»);
  - `case SDL_TEXTINPUT:` → `TextInputHook::ReportText(event->text.text)`;
  - `SDLK_BACKSPACE` (с гвардом `HasActiveConsumer()`) → `ReportBackspace()`;
  - **обязателен `break` после `case SDL_KEYDOWN`** — без него события
    `SDLK_x` «проваливаются» в `SDL_TEXTINPUT` с невалидными `event->text`,
    что даёт падения/мусор в буфере.

---

## 4. Типичные ошибки и как их избегать

| Симптом | Причина | Решение |
|--------|---------|---------|
| «Ничего не появляется» при входе в оверлей | Пишем в скрытый текст, который *не рисуется* на странице, или возвращается `NULL` (`GetText`) | Убедись, что текст существует на странице и видим; используй `SetVisible(true)` |
| «Белый `MULTIPLAYER`» вместо полного промпта | Bootstrap-текст очень маленький шрифтом; `TEXT_WRAP` обрезает/переносит по пробелам | `SetBoundingBoxSize` + `SetPositionOfCenter` + `TEXT_OVERLAP`; при желании `ScaleAboutCenter` |
| Промпт обрезается по краю экрана | Бокс больше окна / позиция вне экрана | Уменьши бокс, сдвинь `SetPositionOfCenter` в центральную/нижнюю зону |
| `SetString(0, buf)` ничего не меняет | Это **MultiText**, показывается текущий `SetIndex`, а мы пишем не в тот слот | `SetIndex(0)` (или тот индекс, который рисуем) + писать в тот же индекс |
| Текст в пункте меню, меню «скачет» / кнопки перескакивают | Пишем в пункт меню; `UpdateCurrentSelection` перезаписывает строку/пульсирует размер | Использовать отдельный от меню текст; пункты меню не трогать |
| Enter «не с первого раза» | Ложное «проглатывание» первого `SELECT` (`ignoreFirstSelect`) | Открывать оверлей из `GUI_MSG_MENU_SELECTION_MADE`, а не ловить `SELECT` в обработчике; `ignoreFirstSelect` не нужен |
| Ввод текста не работает вообще | Не вызван `SDL_StartTextInput()` или `case SDL_KEYDOWN` без `break` | См. §3 |
| Краш в настройках контроллера при открытии (`RealController.cpp:152`) | `GetController(-1)` из-за unset controller ID | Гварды: `controllerID != -1`, `mControllerId >= 0 && < MaxControllers` |
| Ассерт `simutility.cpp:153` при выходе из игры (в MP) | Не сброшен загруженный сейв → SimEnvironment не выгрузился | `ResetGame() + ClearRewards()`, если `IsGameLoaded()`, перед входом в MP |
| Мир загружается без играбельного персонажа | `SetCurrentMission` не вызван | `GetCharacterSheetManager()->SetCurrentMission(L1, M1)` по пути New Game |
| 📌 `GetGameplayManager()` == NULL / AV при входе в геймплей | Прямой `SetContext(CONTEXT_LOADING_GAMEPLAY)` | Входить через `GUI_MSG_QUIT_FRONTEND, 1` — он ставит `GameplayManager` |

### Правила борьбы с крашами

- Даже в Debug игра может умереть без диалога/ассерта (тихий выход). Настроен
  **краш-хендлер** (`win32main.cpp`): пишет символьный стек (DbgHelp) в
  `crash.log` в рабочей папке. Ищи `crash.log` ПЕРВЫМ.
- При отладке наружных симптомов добавляй временный `fprintf` в `mp_dbg.log` в
  точках входа и **убирай его** перед коммитом.

---

## 5. Позиционирование и масштаб текста (Шпаргалка API)

Ключевые методы `Scrooby::Text` / `Scrooby::BoundedDrawable`:

- `SetBoundingBoxSize(w, h)` / `GetBoundingBoxSize()` — бокс, в который
  укладывается/режется текст.
- `SetPositionOfCenter(x, y)` — поставить центр бокса в позицию (координаты
  Scrooby: y вниз от верха).
- `SetHorizontalJustification` / `SetVerticalJustification` — `Left/Centre/Right`
  и `Top/Centre/Bottom`.
- `SetTextMode` — `TEXT_OVERLAP` (не переносить), `TEXT_WRAP`, `TEXT_CLIP`.
- `SetIndex(n)` / `SetString(index, str)` — для MultiText: индекс — слот строки.
- `ScaleAboutCenter(f)` — увеличить визуально маленький ассет-шрифт.
- `SetVisible(true/false)`.

Полезно открыть реализацию `src/libs/scrooby/src/FeText.cpp` и
`FeBoundedDrawable.cpp` — там вся логика нормализации координат и обрезки.

---

## 6. Когда нужен свой `.p3d` (и как это делать)

Новый `.p3d` нужен, если готового скрытого текста/спрайта в ассете нет (например,
собственная кнопка с уникальной текстурой, фон лобби, более одного поля ввода).

Варианты:
1. **Внешний Lucas' Pure3D Editor** — редактировать/создавать `.p3d` вне кода.
2. **Свой чанк-писатель** — написать writer по образцу `LoadScreenChunk` /
   `LoadPageChunk` / `LoadTextChunk` в `FeProjectChunkHandler.cpp`. Это
   позволит генерировать ассеты кодом (как сделано для тестового
   `LobbyRuntimeTest`, который подменял Scrooby-проект — но осторожно: там был
   AV на этапе загрузки в реальном движке).

**Предупреждение:** подменять Scrooby-проект фронтенда в рантайме рискованно —
это даёт AV, если новый проект несовместим по слою/тексту. Предпочитай
переиспользовать существующие элементы, пока UI не станет полноценным
собственным слоем.

---

## 7. Чек-лист перед коммитом UI-фичи

- [ ] Тесты на изменяемый функционал написаны и зелёные (см. `SPEC.ru.md` §13;
      30/30 сейчас).
- [ ] `crash.log` и `mp_dbg.log` не содержат следов отладки; временные
      `fprintf` удалены.
- [ ] Оверлей не ломает навигацию меню (кнопки не «скачут»).
- [ ] Enter подтверждает с первого нажатия (нет лишнего `ignoreFirst`).
- [ ] Ввод текста работает (проверь `SDL_StartTextInput` и `break` после
      `SDL_KEYDOWN`).
- [ ] Сборка: `MSBuild.exe src/srr2/SRR2.vcxproj /p:Configuration=DebugWindows
      /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.22621.0`.

---

## 8. Известные границы / дальнейшие улучшения

- Оверлей ввода IP — это **промпт поверх меню**, а не отдельное диалоговое
  окно с полем. Нет визуального «поля ввода», курсора, кнопок.
  Запланировано: свои экраны + генератор `.p3d` / собственный UI-слой.
- Редкий флейк «загрузка мира застревает у конца» при входе хостом — НЕ баг
  сети (сетевой `Update` неблокирующий, вызывается после загрузки),
  а интермиттентный флейк загрузки уровня. Лечится перезапуском. Подробности в
  `SPEC.ru.md` §15.