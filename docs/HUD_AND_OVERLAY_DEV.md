# Custom HUD / Overlay development in the engine's UI layer

> **Status:** experience documentation from the team/assistant — how to add and
> edit interface elements (overlay/HUD) in the engine, where it usually
> breaks, and how to fix it. English version; Russian — `HUD_AND_OVERLAY_DEV.ru.md`.
>
> See also `SPEC.md` (sections §14–16) and `README.md`.

This article is a practical guide to working with the engine's UI
(menus, HUD, overlays). It comes from the real experience of
adding the "MULTIPLAYER" item and an IP-input overlay to the main menu, and it
should save time for anyone who wants to write their own screens or HUD elements.

---

## 1. How the engine UI works (quick overview)

All interface graphics (menus, HUD, title screens) are **Scrooby** projects that
compile into **Pure3D** (`.p3d`) files. Hierarchy:

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

Key facts about `.p3d`:

- The format is **fully decoded** in our parser sources:
  `src/libs/scrooby/src/FeProjectChunkHandler.cpp` (`LoadScreenChunk`,
  `LoadPageChunk`, `LoadLayerChunk`, `LoadTextChunk`, `LoadSpriteChunk`,
  `LoadGroupChunk`), plus `FeProjectLoader.cpp`, `FeScreen.cpp`, `FePage.cpp`,
  `FeLayer.h`. That is enough for reading/patching assets.
- **There is NO `.p3d` serializer/writer in the Scrooby runtime** — read-only
  (`tChunkFile`). To create your own `.p3d`, either use the external
  **Lucas' Pure3D Editor**, or write your own chunk writer modeled on the
  `Load*` functions.
- The external
  [machinesinmotion/Pure3DFiles](https://github.com/machinesinmotion/Pure3DFiles)
  (donutteam/docs) documentation is incomplete (no chunk header, no endianness)
  and inferior to our sources.

---

## 2. "Code-only, no new assets" approach (recommended)

For new elements (multiplayer overlay, hints, custom HUD) **you do not need to
generate a new `frontend.p3d`**. All you need is to take an existing hidden or
service element in the asset and reuse it from code:

- Find a **hidden `Text`** on the target page that is **not** a menu item
  (e.g. `MainMenu` vs `MainMenu_PC` in the main menu on Windows).
- Pull it from the `CGuiScreen*`:
  ```cpp
  Scrooby::Text* t = dynamic_cast<Scrooby::Text*>( page->GetText( "MainMenu" ) );
  ```
- Configure geometry/alignment from code:
  ```cpp
  t->SetVisible( true );
  t->SetIndex( 0 );                        // which MultiText string to show
  t->SetTextMode( Scrooby::TEXT_OVERLAP ); // do not wrap on spaces
  t->SetBoundingBoxSize( 800, 120 );
  t->SetHorizontalJustification( Scrooby::Centre );
  t->SetVerticalJustification( Scrooby::Centre );
  t->SetPositionOfCenter( 400, 240 );
  // ... then:
  t->SetString( 0, "MULTIPLAYER HOST\nAddress: ..." );
  ```

That gives a fully readable prompt over the menu **without a single new asset**
and without breaking navigation.

### Why a hidden / separate text

- Menu items are **MultiText with one string per selection index**, and the menu
  overwrites them (`SetString`, colour/size pulse every frame).
- If you write the overlay **into a menu item**, then:
  - a long string breaks the item's layout;
  - `UpdateCurrentSelection` pulses size via `ResetTransformation() +
    ScaleAboutPoint()` — the text "jumps";
  - you can break navigation (menu cycles selections, keys "skip around").
- Therefore write into a text that is separate from the menu.

---

## 3. Text input in the overlay (`TextInputHook`)

The game by default does **not** feed keyboard text into GUI screens (input is
controller-oriented). For IP/port entry in the overlay a tiny bridge was made:

- Files: `src/game/presentation/gui/utility/textinputhook.{h,cpp}`.
- Idea: a global hook with **one active consumer**:
  ```cpp
  TextInputHook::SetActive( OnMpOverlayChar, OnMpOverlayBackspace );
  TextInputHook::ClearActive();
  TextInputHook::HasActiveConsumer();
  ```
- Platform part (`win32platform.cpp`) in the SDL loop:
  - `SDL_StartTextInput()` when initializing the window — **without this SDL
    never sends `SDL_TEXTINPUT`** (typical bug: "typing does nothing");
  - `case SDL_TEXTINPUT:` → `TextInputHook::ReportText(event->text.text)`;
  - `SDLK_BACKSPACE` (guarded by `HasActiveConsumer()`) → `ReportBackspace()`;
  - **a `break` after `case SDL_KEYDOWN` is mandatory** — without it `SDLK_x`
    events "fall through" into `SDL_TEXTINPUT` with invalid `event->text`,
    causing crashes/garbage in the buffer.

---

## 4. Common mistakes and how to avoid them

| Symptom | Cause | Fix |
|---------|-------|-----|
| "Nothing appears" when entering the overlay | Writing into a hidden text that is *not drawn* on the page, or `GetText` returns `NULL` | Make sure the text exists on the page and is visible; use `SetVisible(true)` |
| "White `MULTIPLAYER`" instead of the full prompt | The bootstrap text has a tiny font; `TEXT_WRAP` clips/wraps on spaces | `SetBoundingBoxSize` + `SetPositionOfCenter` + `TEXT_OVERLAP`; optionally `ScaleAboutCenter` |
| Prompt clipped at the screen edge | Box larger than the window / position off-screen | Shrink the box, move `SetPositionOfCenter` toward the center/lower zone |
| `SetString(0, buf)` changes nothing | It's a **MultiText**: the current `SetIndex` is shown, we wrote the wrong slot | `SetIndex(0)` (or the index we render) + write to the same index |
| Text inside a menu item, menu "jumps"/keys skip | Writing into a menu item; `UpdateCurrentSelection` overwrites the string / pulses size | Use a text separate from the menu; do not touch menu items |
| Enter "not on the first press" | Falsely swallowing the first `SELECT` (`ignoreFirstSelect`) | Open the overlay from `GUI_MSG_MENU_SELECTION_MADE` instead of catching `SELECT` in the handler; `ignoreFirstSelect` is unnecessary |
| Text input does not work at all | `SDL_StartTextInput()` not called, or `case SDL_KEYDOWN` without `break` | See §3 |
| Crash opening controller settings (`RealController.cpp:152`) | `GetController(-1)` from an unset controller ID | Guards: `controllerID != -1`, `mControllerId >= 0 && < MaxControllers` |
| Assert `simutility.cpp:153` on game exit (in MP) | Loaded save not reset → SimEnvironment not unloaded | `ResetGame() + ClearRewards()` if `IsGameLoaded()`, before entering MP |
| World loads without a playable character | `SetCurrentMission` not called | `GetCharacterSheetManager()->SetCurrentMission(L1, M1)` following the New Game path |
| 📌 `GetGameplayManager()` == NULL / AV when entering gameplay | Direct `SetContext(CONTEXT_LOADING_GAMEPLAY)` | Enter via `GUI_MSG_QUIT_FRONTEND, 1` — it sets up `GameplayManager` |

### Crash-fighting rules

- Even in Debug the game can die without a dialog/assert (silent exit). A
  **crash handler** is configured (`win32main.cpp`): it writes a symbolic stack
  (DbgHelp) to `crash.log` in the working folder. Check `crash.log` FIRST.
- When debugging external symptoms, add temporary `fprintf` to `mp_dbg.log` at
  entry points and **remove it before committing**.

---

## 5. Positioning and scaling text (API cheat sheet)

Key methods of `Scrooby::Text` / `Scrooby::BoundedDrawable`:

- `SetBoundingBoxSize(w, h)` / `GetBoundingBoxSize()` — the box the text is laid
  out/clipped into.
- `SetPositionOfCenter(x, y)` — place the box center at a position (Scrooby
  coords: y grows downward from the top).
- `SetHorizontalJustification` / `SetVerticalJustification` — `Left/Centre/Right`
  and `Top/Centre/Bottom`.
- `SetTextMode` — `TEXT_OVERLAP` (no wrap), `TEXT_WRAP`, `TEXT_CLIP`.
- `SetIndex(n)` / `SetString(index, str)` — for MultiText: the index is the
  string slot.
- `ScaleAboutCenter(f)` — visually enlarge a small asset font.
- `SetVisible(true/false)`.

It helps to open `src/libs/scrooby/src/FeText.cpp` and
`FeBoundedDrawable.cpp` — they contain all the coordinate-normalization and
clipping logic.

---

## 6. When you need your own `.p3d` (and how to do it)

A new `.p3d` is needed when there is no existing hidden text/sprite in the asset
(for example, a custom button with a unique texture, a lobby background, more
than one input field).

Options:
1. **External Lucas' Pure3D Editor** — edit/create `.p3d` outside the code.
2. **Own chunk writer** — write a writer modeled on `LoadScreenChunk` /
   `LoadPageChunk` / `LoadTextChunk` in `FeProjectChunkHandler.cpp`. That lets
   you generate assets from code (as was done for the test `LobbyRuntimeTest`
   that swapped the Scrooby project — but carefully: it AV'd at load time in the
   real engine).

**Warning:** swapping the front-end Scrooby project at runtime is risky — it AVs
if the new project is incompatible in layer/text. Prefer reusing existing
elements until the UI becomes a full custom layer.

---

## 7. Pre-commit checklist for a UI feature

- [ ] Tests for the changed functionality are written and green (see `SPEC.md`
      §13; currently 30/30).
- [ ] No debug traces left in `crash.log` / `mp_dbg.log`; temporary `fprintf`s
      removed.
- [ ] The overlay does not break menu navigation (keys do not "skip around").
- [ ] Enter confirms on the first press (no extra `ignoreFirst`).
- [ ] Text input works (check `SDL_StartTextInput` and the `break` after
      `SDL_KEYDOWN`).
- [ ] Build: `MSBuild.exe src/srr2/SRR2.vcxproj /p:Configuration=DebugWindows
      /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.22621.0`.

---

## 8. Known limitations / future work

- The IP-input overlay is a **prompt over the menu**, not a separate dialog with
  an input field. There is no visible "input field", cursor, or buttons. Planned:
  custom screens + `.p3d` generator / a custom UI layer.
- The rare "world load sticks near the end" flake when entering as host is NOT a
  network bug (the network `Update` is non-blocking and runs after the world
  loads), but an intermittent level-load flake. It is fixed by restarting. See
  `SPEC.md` §15 for details.