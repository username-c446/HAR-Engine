# The Simpsons: Hit & Run (SRR2) — Open-Source Rebuild

> **[Русская версия / Russian version →](README.ru.md)**

This repository contains the source code for _The Simpsons: Hit & Run_ with
quite a few enhancements to it: C++20 & x64 builds, vcpkg for easier third-party
library management, UWP builds, and a new **online multiplayer** layer built on
top of ENet.

> Looking for what changed versus the original game? See
> **[Changes vs. original](#changes-vs-original)**. For a deep dive into the
> multiplayer architecture, read **[SPEC.md](./SPEC.md)**.

---

## Disclaimer

This repository contains modified source code and references to original game
assets for preservation and enhancement purposes only.

- This project is **not affiliated with, endorsed by, or sponsored by** the original publishers or intellectual property owners.
- No ownership of the original intellectual property is claimed.
- This project is **not monetized** in any way (no sales, ads, sponsorships, or donations).
- Commercial redistribution of this project or its builds is not permitted.

_The Simpsons: Hit & Run_ and all related characters, assets, audio, trademarks,
and branding remain the property of their respective rights holders.

---

## Changes vs. original

This fork extends the base source with a from-scratch multiplayer layer and
several quality-of-life improvements. The full technical breakdown lives in
[SPEC.md](./SPEC.md); here is the high-level list:

### Multiplayer (new)

- **Networking stack on ENet** (`src/game/network/`): reliable client/server
  layer with a server-authoritative model — the server admits players, assigns
  `clientId`s, validates movement and relays states. No peer-to-peer assumption.
- **Host / join lifecycle**: console commands (`host`, `join`, `match`) plus
  command-line auto-start (`HOSTPORT=`, `JOINIP=`, `HOSTPASS=`, `HOSTSLOTS=`, `JOINPASS=`).
- **Remote player representation**: networked player characters and driver
  "ghost" vehicles are spawned and interpolated smoothly in the world
  (`RemotePlayerWorldView`, `RemoteVehicleWorldView`).
- **Player list & presence**: the server broadcasts an authoritative entity
  list; clients see connect/disconnect notifications.
- **Heartbeat & liveness**: PING/PONG keeps track of latency (RTT) and lets the
  server drop silent clients instead of keeping stale slots.
- **Server-side Lua mods**: scripts can hook join/validation/leave decisions
  (`ServerModManager`).
- **Dedicated server**: headless console server (`mp_standalone`) with optional
  password, max-slot limit, and file logging.

### Engine / build

- C++20 & x64 builds.
- vcpkg for third-party dependencies (`enet`, `lua`, `sdl2`, …).
- UWP build targets retained.
- GitHub Actions workflow (`build.yml`) for CI builds.

### Stability fixes

- Several startup-path fixes for the Windows desktop build (SDK/runtime issues,
  dynamic-library loading) to get the game running from source.
- Test suite under `src/tests/` covering the network layer end-to-end.

> The original game only shipped as a retail product; **game data/assets are not
> included** in this repository. You must supply the original game files yourself.

---

## Contributing

Contributions are welcome! There are some pretty bad bugs that need fixing, such
as the UWP memory leak.

To contribute, please [fork this repository](https://github.com/username-c446/SRR2-Open-Rebuild/fork)
and [open a pull request](https://github.com/username-c446/SRR2-Open-Rebuild/pulls).

## Issues

If you encounter issues, please [create an issue](https://github.com/username-c446/SRR2-Open-Rebuild/issues/new).

### Known Bugs

- Memory Corruption
- Assets (need a good way to include them)

## Installation

### Quick Installation (Pre-built Binaries)

> [!NOTE]
> Binaries are only available if a build has been published to Releases.

### Desktop

1. Download the latest build from the [Releases page](https://github.com/username-c446/SRR2-Open-Rebuild/releases/latest).
2. Extract the contents of the zip file to your desired location.
3. Navigate to the extracted folder and locate `SRR2.exe`.
4. You can now run the game.

### Xbox Series S|X

> [!WARNING]
> Xbox One is currently not supported and will crash; Xbox Series S|X is fine.

1. Navigate to the [Releases page](https://github.com/username-c446/SRR2-Open-Rebuild/releases/latest).
2. Download a file that looks like `SRR2_UWP_X.X.X.X_x64_XXX.appx`.
3. Now navigate to the Xbox dev mode portal (https://xbox:11443/ or the local IP and port the dev mode dashboard shows you).
4. Then press "Add" and drag and drop the AppX you downloaded.
5. Once done it should run.

### Developer Installation (Building from Source)

When working with the source you will need these installed:

- Visual Studio 2026
- C++ Language Support (in the Visual Studio installer, select the `Desktop development with C++` and `Universal Windows Platform development` options)
- Vcpkg support (when selecting `Desktop development with C++`, make sure `Vcpkg package manager` is selected on the right side)

#### Setup

> [!NOTE]
> If you have already built vcpkg projects before with Visual Studio, you can skip everything past step 3.

1. Open a command prompt and navigate to a directory where you want to store the source, then run `git clone --recurse-submodules https://github.com/username-c446/SRR2-Open-Rebuild`
2. When done, open `SRR2.sln` with Visual Studio.
3. In Visual Studio, press `Tools -> Command Line -> Developer Command Prompt`.
4. A Developer Command Prompt will open; enter `cd tools/vcpkg` and then `./bootstrap-vcpkg.bat`.
5. Once done, run `./vcpkg integrate install`.

#### Building

If the setup was successful, you should now be able to build any project in the
solution. When building for UWP, don't forget to change the config (e.g. from
`ReleaseWindows` to `ReleaseUwp`).

## Media

### Windows

![Screenshot 2025-02-10 092453](https://github.com/user-attachments/assets/7b5c9c6a-259d-4e5d-bd07-e429bd2f54bb)

### Xbox

Watch [HD on YouTube](https://www.youtube.com/watch?v=qxqnziUVz9c) —
https://github.com/user-attachments/assets/9793dccf-5dd6-4bbf-beb6-a6db33521a0b

Watch [HD on YouTube](https://www.youtube.com/watch?v=l_Ii-4Wygn8) —
https://github.com/user-attachments/assets/ccfdb377-10ed-418b-a81b-932aad9938e1

---

## Credits & Original Author

This work is based on the openly released source of _The Simpsons: Hit & Run_
rebuilt by the community. The original modern rebuild was created by
[**3UR**](https://github.com/3UR) at
[github.com/3UR/Simpsons-Hit-Run](https://github.com/3UR/Simpsons-Hit-Run),
which in turn derives from the leaked/archived original source. All credit for
the base rebuild belongs to them; this repository adds the multiplayer layer and
fixes on top of that foundation.