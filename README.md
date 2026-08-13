# Minimize Effect for Windows

**Minimize Effect** (`Minimize-Effect-Windows`) is an open-source Windows desktop app that replaces the stock minimize and restore transition with **smooth mesh animations** into the taskbar (classic / curvy minimize-style curves and squash). When you minimize a window, the live desktop region is captured and warped until it lands on the taskbar target (and expands back out on restore).

It is a native **C++ / Direct3D 11 / DirectComposition** project with a polished ImGui settings UI — not a shell theme pack or AutoHotkey script.

| | |
| --- | --- |
| **Platform** | Windows 10 / 11 (x64) |
| **License** | [MIT](LICENSE.txt) |
| **Language** | C++ (latest MSVC) |
| **UI** | Dear ImGui + FreeType (Inter) |
| **Graphics** | D3D11, DXGI Desktop Duplication, DirectComposition |

---

## Features

- **Custom minimize & restore** — mesh-based deformation toward the taskbar (or a custom rect)
- **GPU-resident capture path** — direct GPU-to-GPU `CopySubresourceRegion` texture transfers without CPU staging copies
- **Precompiled HLSL shaders** — shaders precompiled into C++ headers at build time (no runtime `D3DCompile` or `d3dcompiler_47.dll` dependency)
- **In-memory window state** — thread-safe `unordered_map` state storage (zero Win32 `SetPropW` kernel atom table pollution)
- **DWM Native dragging** — `WM_NCHITTEST` returning `HTCAPTION` for smooth window movement and Windows 11 Snap Layouts
- **Concurrent animations** — multiple windows can animate without blocking each other
- **Separate motion controls** — minimize vs restore duration, linked or independent speeds
- **Named motion profiles** — save and reapply complete timing, curve, style, and quality setups
- **Easing & style options** — presets, custom cubic-bezier, classic / curvy / squash, strength, fade
- **Automatic quality** — adaptive mesh density under load and resolution pressure (8-bit R8 mask textures save 75% VRAM)
- **App exclusions** — skip the effect for specific executables
- **System integration** — run at startup, start minimized, tray icon, close-to-tray or exit (managed with Microsoft WIL)
- **Hotkeys** — toggle the effect, open settings, repair windows (configurable)
- **Settings UI** — dark macOS-inspired shell (traffic lights, sidebar, cards, motion)
- **Safe settings recovery** — one-step undo/redo plus an automatically maintained backup
- **Repair / diagnostics** — status for effect, hook, renderer, D3D device, display, and administrator restart
- **Native animation suppression** — disables classic shell + DWM transitions while running
- **Device-lost recovery** — recreates capture/overlay/settings renderers after GPU resets
- **Opt-in software updates** — in-process zip extraction (`miniz`), WinHTTP client, `PicoSHA2` hashing, and `nlohmann::json`

---

## How it works (high level)

Windows does **not** expose a public API that means “replace this DWM minimize animation before the compositor runs it.” Minimize Effect uses the strongest **documented** path available:

1. **Detect** minimize/restore via WinEvents and a **CBT hook DLL** (`MinimizeEffectHook.dll`).
2. **Policy** decides whether the effect applies (enabled, pause, fullscreen, battery saver, event-driven power setting notifications, exclusions).
3. **Suppress** the stock transition per window with `DwmSetWindowAttribute(DWMWA_TRANSITIONS_FORCEDISABLED)`, then restore that override during effect cleanup.
4. **Capture** the visible window region via **DXGI Desktop Duplication** directly into GPU VRAM (`ID3D11Texture2D`) without CPU Map/Unmap staging buffers.
5. **Composite** a transparent topmost overlay with **DirectComposition** (`wil::com_ptr`) + a D3D11 swap chain.
6. **Deform** a textured mesh using precompiled vertex/pixel shaders (Minimize curve / squash) each frame until the window lands at the taskbar target.

Elevated processes are only visible to the hook if Minimize Effect itself runs elevated (UIPI).

For a deeper technical write-up, see [`docs/architecture.md`](docs/architecture.md).

---

## Requirements

### Run

- Windows 10 or Windows 11 (64-bit)
- A GPU with Direct3D 11 support
- Desktop Duplication available on the target session (normal interactive desktop)

### Build

- **Visual Studio 18** or the matching **Build Tools** with:
  - Desktop development with C++
  - MSBuild
  - MSVC `v145` toolset
- **x64** platform only
- No separate vcpkg step for core deps — **ImGui** and **FreeType** are vendored under `app/third_party/`

---

## Build

### Visual Studio

1. Open `MinimizeEffect.slnx`
2. Select configuration **Release** (or **Debug**) and platform **x64**
3. Build **Solution** (`Ctrl+Shift+B`)

The app project builds the hook DLL first, then links/embeds it as needed.

### Command line (Developer PowerShell)

```powershell
MSBuild.exe MinimizeEffect.slnx /p:Configuration=Release /p:Platform=x64 /m
```

### Outputs

| Path | Contents |
| --- | --- |
| `build\bin\x64\Release\` | `MinimizeEffect.exe`, `MinimizeEffectHook.dll` (+ PDBs) |
| `build\bin\x64\Debug\` | Debug binaries |
| `build\obj\App\x64\<Config>\` | App intermediates |
| `build\obj\Hook\x64\<Config>\` | Hook intermediates |

**Runtime:** keep `MinimizeEffect.exe` and `MinimizeEffectHook.dll` in the **same folder**. Release builds can also use the embedded hook resource when configured with `MINIMIZE_EMBED_RELEASE_HOOK`.

---

## Usage

1. Build (or obtain) a Release binary pair.
2. Run `MinimizeEffect.exe` (optionally **as Administrator** if you want the effect on elevated windows).
3. Open the settings window from the tray or hotkey.
4. Enable the effect (sidebar status chip shows **On** / **Off** / **Paused**).
5. Minimize any eligible window — it should Minimize into the taskbar.

### Settings overview

| Page | What you control |
| --- | --- |
| **Effect** | Master enable, close behavior, startup options |
| **Motion** | Durations, easing, custom bezier, style, quality, strength, fade, preview |
| **Apps** | Exclude executables from the effect |
| **System** | Windows integration helpers |
| **Hotkeys** | Global shortcuts |
| **Repair** | Live diagnostics (hook, renderer, display, etc.) |
| **About** | Product version, licenses |

### Software updates

Minimize Effect checks the repository's latest stable GitHub Release in the background. It never
installs an update automatically:

1. A small animated card and, when the app is in the tray, a Windows notification announce a new
   version.
2. Press **Update now** to download it, or **Later** to keep using the current version.
3. The existing settings window becomes the update workspace: its content animates away while
   the traffic-light window controls remain fixed. Download, verification, staging, and retry
   states are rendered there through the shared motion system.
4. The verified files are swapped transactionally. The replacement process paints the same
   update frame at the exact window bounds before the old process exits, then restores the
   selected page, scroll position, maximized state, and normal content without opening a visible
   helper window. If handover fails, the running version rolls back the files in place.

The updater uses only the public releases from
`ByteX420/Minimize-Effect-Windows`; it requires no account, token, or background service. Update
status and a manual **Check again** action also live on the **About** page.

Settings persist to:

```text
%LOCALAPPDATA%\MinimizeEffect\settings.json
```

---

## Configuration & environment

### Environment variables

| Variable | Purpose |
| --- | --- |
| `MINIMIZE_TASKBAR_RECT` | Override minimize target as `left,top,right,bottom` (physical screen coords). Useful for custom taskbars. |
| `MINIMIZE_DEBUG_LOG` | Override path of the debug log file |
| `MINIMIZE_TRACE=1` | Verbose timing traces (noisy; for debugging) |
| `MINIMIZE_LOG_SYNC=1` | Flush every log line (helps after hangs/crashes) |
| `MINIMIZE_TEST_DEVICE_RECOVERY=1` | Debug: one controlled D3D teardown/recreate after startup |

Example custom taskbar target:

```powershell
$env:MINIMIZE_TASKBAR_RECT = "100,980,1820,1070"
.\MinimizeEffect.exe
```

### Debug log

Debug builds write diagnostics to:

```text
%LOCALAPPDATA%\MinimizeEffect\minimize_debug.log
```

---

## Project layout

```text
Minimize-Effect-Windows/
|-- MinimizeEffect.slnx
|-- app/
|   |-- MinimizeEffect.vcxproj
|   |-- MinimizeEffect.rc
|   |-- assets/fonts/
|   |-- shaders/
|   |-- src/
|   |   |-- main.cpp
|   |   |-- animation/            # Mesh geometry and easing (platform-free)
|   |   |-- app/                  # Composition root, lifecycle, message loop
|   |   |-- core/                 # Logger, environment, embedded resources
|   |   |-- features/             # Policy, minimize/restore, pause, diagnostics
|   |   |-- platform/windows/     # Win32/DWM/shell/hook/hotkey adapters
|   |   |-- rendering/            # D3D device, capture, overlay draw path
|   |   |-- runtime/              # Animation runs, state, pacing, recovery
|   |   |-- settings/             # Model, validation, serializer, repository
|   |   `-- ui/                   # Settings host, shell, tray, preview
|   |       |-- pages/            # Effect, Motion, Apps, Displays, System, Hotkeys, Repair, About
|   |       |-- components/       # Controls, combo, easing editor, layout
|   |       |-- theme/            # Visual tokens and chrome
|   |       |-- motion/           # UI motion system
|   |       `-- rendering/        # ImGui/D3D settings renderer
|   `-- third_party/              # Vendored ImGui + FreeType
|-- hook/                         # MinimizeEffectHook.dll (CBTProc)
|-- docs/
|   `-- architecture.md
|-- LICENSE.txt
`-- README.md
```

### Code style

- C++ style follows **Google C++** conventions via [`.clang-format`](.clang-format) (2-space indent, 100 columns).
- Format first-party sources (not `third_party/`):

```powershell
$cf = "…\clang-format.exe"   # e.g. VS LLVM x64 clang-format
Get-ChildItem app\src, hook -Recurse -Include *.cpp,*.hpp,*.h |
  ForEach-Object { & $cf -i --style=file $_.FullName }
```

---

## Architecture notes for contributors

Dependencies point inward:

```text
main -> app -> features / runtime / ui -> rendering / platform / settings -> core / animation
```

| Layer | Responsibility |
| --- | --- |
| `core/` | Logging, environment flags, embedded resources |
| `animation/` | Pure mesh, geometry, and easing |
| `platform/windows/` | Window events, DWM, hooks, process/shell adapters |
| `rendering/` | Device, desktop duplication, mesh and overlay rendering |
| `settings/` | Model, validation, serializer, repository, service |
| `runtime/` | Runs, state machine, pacing, snapshots, recovery |
| `features/` | Policy, minimize/restore, pause, diagnostics, mutations |
| `ui/` | Win32 host, ImGui renderer, shell, pages, components, tray |
| `app/` | Composition root, lifecycle, message loop |
| `hook/` | Separate CBT DLL and stable app/DLL boundary |

**Important limitations (by design):**

- No official “pre-DWM replace animation” API — behavior can vary with shell updates.
- **UIPI:** non-elevated Minimize Effect cannot hook elevated windows.
- Multi-monitor / exotic taskbar setups may need `MINIMIZE_TASKBAR_RECT`.
- Fullscreen games / exclusive modes may disable or skip the effect (settings flags exist for battery saver / fullscreen-related behavior).

See [`docs/architecture.md`](docs/architecture.md) for ownership, state machine, and recovery paths.

---

## Automated releases (GitHub Actions)

The repository has two public release tracks:

- **Stable:** a version change pushed to `stable` creates `vX.Y.Z` and remains the app updater's
  only update source.
- **Pre-release:** changing `.github/BETA_VERSION` and pushing it to `beta` publishes exactly the
  entered version, such as `v1.5.0-beta.1`, `v1.5.0-beta.rc`, or `v1.5.0-rc.1`.

Both tracks use the same cached **Release | x64** build implementation. Pre-releases are marked as
such on GitHub, are never made the latest release, and use a versioned ZIP filename so testers can
keep multiple builds.

Workflow files: [`.github/workflows/release.yml`](.github/workflows/release.yml) and
[`.github/workflows/beta-release.yml`](.github/workflows/beta-release.yml).

### Publish a beta or release candidate

1. Finish and commit the code for the pre-release on `dev`.
2. As the final release change, edit the single line in `.github/BETA_VERSION`:

   ```text
   1.5.0-beta.1
   ```

3. Commit the version and push `dev` to `beta`:

   ```powershell
   git add .github/BETA_VERSION
   git commit -m "chore: release 1.5.0-beta.1"
   git push origin dev
   git push origin dev:beta
   ```

4. Normal code pushes that do not change `BETA_VERSION` create no release. For the next public
   build, manually change the line to `1.5.0-beta.2`. For a release candidate, enter
   `1.5.0-beta.rc`, `1.5.0-rc`, `1.5.0-rc.1`, and so on.

The exact `BETA_VERSION` value is used for the Git tag, release title, ZIP filename, Windows
ProductVersion shown on the About page, and the Windows pre-release flag. These resource changes
exist only inside the runner; `app/MinimizeEffect.rc` remains the independent stable version.

### Cut a new release

1. On `dev` (or a branch), bump the version macros in `app/MinimizeEffect.rc`:

   ```c
   #define MINIMIZE_FILE_VERSION      1,5,2,0
   #define MINIMIZE_PRODUCT_VERSION   1,5,2,0
   #define MINIMIZE_FILE_VERSION_STR  "1.5.2\0"
   #define MINIMIZE_PRODUCT_VERSION_STR "1.5.2\0"
   ```

2. After testing on `beta`, merge into **`stable`** and push:

   ```powershell
   git checkout stable
   git merge dev
   git push origin stable
   ```

3. Actions runs automatically (because `MinimizeEffect.rc` changed). Check the **Actions** tab, then **Releases**.

Manual run: **Actions → Release → Run workflow** (optional version override).

### Security (no leaked tokens)

| What | How |
| --- | --- |
| Auth | Built-in **`GITHUB_TOKEN` only** — never a personal access token in the repo |
| Permissions | Workflow requests only `contents: write` (tags + release assets) |
| Triggers | Version-related push to **`stable`**, or a `BETA_VERSION` change on **`beta`** — **not** on pull requests |
| Forks | Fork PRs cannot publish releases to this repository |
| Secrets in code | None required for this workflow |

You do **not** put your GitHub password, PAT, or SSH key into the project. The token exists only for that job run and is scoped by GitHub.

---

## Contributing

Contributions are welcome. A good PR:

1. **Builds** cleanly on x64 Release and Debug.
2. **Formats** first-party C++ with the repo `.clang-format`.
3. **Keeps** platform-independent animation code free of Win32 when possible.
4. **Documents** user-facing behavior changes in the PR description.
5. **Avoids** committing `build/`, local `*.user` noise, or secrets.

### Suggested workflow

```powershell
git checkout -b feature/my-change
# … edit …
MSBuild.exe MinimizeEffect.slnx /p:Configuration=Release /p:Platform=x64 /m
# format changed sources with clang-format
git commit
```

If you change minimize timing or mesh math, mention comparison against stock DWM and any multi-monitor testing you did.

### Reporting issues

Please include:

- Windows version (Win10/11 build number)
- GPU / driver if graphics-related
- Elevated vs normal process
- Steps to reproduce
- Relevant lines from `minimize_debug.log` (Debug builds) if available

---

## Troubleshooting

| Symptom | Things to try |
| --- | --- |
| No animation at all | Confirm effect is **On** in settings; check Repair page (Hook / Renderer / D3D). |
| Elevated apps ignore effect | Run Minimize Effect **as Administrator**. |
| Wrong suck target | Set `MINIMIZE_TASKBAR_RECT` or check taskbar edge (top/bottom/left/right). |
| Black / stuck overlay | Restart app; check device-lost path; update GPU drivers. |
| Hook not installed | Ensure `MinimizeEffectHook.dll` sits next to the EXE; rebuild both projects. |
| Settings not saving | Check write access to `%LOCALAPPDATA%\MinimizeEffect\`. |

---

## Credits & third-party

- **Minimize mesh / timing inspiration** — adapted from Harshil Shah’s MIT-licensed [Minimize](https://github.com/HarshilShah/Minimize) SpriteKit playground  
- **[Dear ImGui](https://github.com/ocornut/imgui)** — immediate-mode UI  
- **[FreeType](https://freetype.org/)** — font rasterization for the settings UI  
- **[Inter](https://github.com/rsms/inter)** — UI typeface (SIL Open Font License; see `app/assets/fonts/OFL.txt`)

Windows, DirectX, and DirectComposition are trademarks of Microsoft Corporation. macOS is a trademark of Apple Inc. This project is not affiliated with Apple or Microsoft.

---

## License

This project is released under the **MIT License**. See [LICENSE.txt](LICENSE.txt).

```text
Copyright (c) 2026 ByteX420
```

You are free to use, modify, and distribute the software, including commercially, provided the license notice is preserved. The software is provided **as is**, without warranty.

---

## Disclaimer

Minimize Effect interacts with window management, accessibility-style event hooks, and desktop capture. Use at your own risk. Behavior may change with Windows updates. Do not use this software to bypass security boundaries or capture content you are not allowed to access.
