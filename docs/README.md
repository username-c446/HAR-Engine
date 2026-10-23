# HAR-Engine

> [🇷🇺 Russian version](README.ru.md)

> The starting point was the game _The Simpsons: Hit & Run_ — only the code
> scaffold and sources were taken as a base. From there on the project evolves as
> an **independent game engine**, whose capabilities go well beyond the modest
> functionality of the original.

This repository is an engine built from source: C++20 & x64, vcpkg for
third-party libraries, cross-platform targets (including UWP), a reusable
networking layer on ENet, and its own content tooling.

> Network architecture — see **[SPEC.md](./SPEC.md)**.
> UI/HUD/overlays — see **[HUD_AND_OVERLAY_DEV.md](./HUD_AND_OVERLAY_DEV.md)**.

---

## What this engine is

From the original game only the engineering core is kept (level loading,
rendering, world objects, vehicles). Everything on top is rebuilt as **engine
modules**, each reusable and extensible:

- **Networking layer** (`src/game/netengine/`): client/server on ENet with a
  server-authoritative model, versioned packets, a handler registry,
  password/authentication, movement validation, relay.
- **Multiplayer**: host/join, console commands and CLI auto-start (`HOSTPORT=`,
  `JOINIP=`, `HOSTPASS=`, `HOSTSLOTS=`, `JOINPASS=`), remote entities with
  interpolation, player list, heartbeat/RTT, a dedicated headless server
  (`mp_standalone`).
- **Content tooling**: a `.p3d` UI-project generator (`P3DWriter`), a runtime UI
  layer (Scrooby), custom HUD/overlays written in plain code.
- **Modding**: server-side scripts (Lua) with admission/validation/leave hooks.
- **Build**: C++20, x64, vcpkg, CI (GitHub Actions).

The guiding principle is **"code, no mandatory assets"**: the engine runs and
works without shipping the original game data, degrading gracefully instead of
crashing.

---

## Data

> Original game data (art, levels, audio) is **not** part of this repository and
> is not required to develop the engine. To run legacy content you must supply it
> separately; the engine itself builds and runs on its own content and stubs.

---

## Build

### From source

Requirements:
- Visual Studio (Desktop development with C++) and the UWP target.
- Vcpkg (select `Vcpkg package manager` in the installer).

1. `git clone --recurse-submodules <url>` and open `SRR2.sln`.
2. In a Developer Command Prompt: `cd tools/vcpkg` → `./bootstrap-vcpkg.bat` → `./vcpkg integrate install`.
3. Build the project you need. For UWP switch config `ReleaseWindows` → `ReleaseUwp`.

Build notes (as tuned on this machine): `MSBuild.exe src/srr2/SRR2.vcxproj
/p:Configuration=DebugWindows /p:Platform=x64 /p:WindowsTargetPlatformVersion=10.0.22621.0`
(the `sim` project targets a missing SDK 10.0.26100.0, so the version is
overridden on the command line).

### Tests

Build: `MSBuild.exe src/tests/Tests.vcxproj "/p:Configuration=Debug"
"/p:Platform=x64" /p:WindowsTargetPlatformVersion=10.0.22621.0`
Run: `vstest.console.exe src\tests\x64\Debug\Tests.dll`. "Done" requires green
tests (currently 42/42).

---

## Contributing

Contributions are welcome. First read `AGENTS.md` (testing rules,
cross-platform guidelines) and `AI_INSTRUCTIONS.md`. Fork and open a PR.

## Issues

If you run into a problem, open an issue and attach `crash.log` (if present) and
your build configuration.

---

## Origin

The base code is a modified/rebuilt source of _The Simpsons: Hit & Run_ released
by the community (the modern rebuild was made by [**3UR**](https://github.com/3UR);
it in turn derives from the archived/leaked source). Credit for the base scaffold
belongs to them. This repository uses that scaffold as a starting point and
develops it as an **independent engine**: all further extension (networking,
tooling, UI, modding) is implemented by our own means, decoupled from the game's
brand and content. Commercial use of the original's assets/brand is prohibited;
the engine as code is open.