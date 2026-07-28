# Minimize Effect architecture

Minimize Effect is a native Windows application that replaces the visible
minimize/restore transition with a Direct3D 11 mesh animation. Windows has no
documented “replace DWM animation” API, so shell integration is isolated behind
Windows adapters and restored during every cleanup path.

## Dependency direction

Dependencies point inward:

```text
main -> app composition -> features/runtime/ui -> rendering/platform/settings -> core/animation
```

- `core/`: logging, environment access, and embedded resources.
- `animation/`: geometry, easing, and mesh generation.
- `settings/`: model, validation, serialization, repository, and service.
- `platform/windows/`: focused Win32, DWM, shell, power, display, hook,
  process, startup, session, and process-lifetime adapters.
- `rendering/`: D3D device, desktop duplication/capture, animation renderer,
  overlay renderer, and overlay HWND/DirectComposition host.
- `runtime/`: animation runs and states, snapshots, frame scheduling, renderer
  recovery, and per-frame runtime coordination.
- `features/`: policy, minimize, restore, pause, diagnostics, settings
  mutations, hotkeys, animation configuration, and window recovery.
- `ui/`: settings host, ImGui renderer, shell, controller/view model, tray,
  preview, pages, components, motion, and theme.
- `app/`: composition/lifecycle and the callback-driven message loop.

`third_party/` remains vendor-owned and is not modified by first-party work.

## Ownership and state

`Application` is a small composition root. Dependencies are constructed in
declaration order and destroyed in reverse order. `CleanupAndRestoreAll` is
idempotent and shared by normal shutdown, failed initialization, cancellation,
timeouts, and renderer failure.

Each animation occupies one `AnimationRun` from `AnimationRunPool`. `RunState`
validates transitions. A run owns its overlay and transient window/capture
state; shared pre-minimize and restore snapshots live in `SnapshotCache`.

Foreign application window state (minimize/restore permissions, transparency, original placement)
is managed centrally in process memory via `g_window_states` (`std::unordered_map<HWND, WindowState>`)
protected by `std::shared_mutex` read/write locking. Win32 `SetPropW` kernel atom injections into third-party
windows are avoided entirely; the CBT hook DLL queries state asynchronously via the `MinimizeQueryWindowState`
registered window message.

The settings UI uses one `SettingsActions` boundary. Pages own presentation
only through `SettingsViewModel`; settings persistence remains outside UI.

## Minimize and restore

```text
WinEvent/CBT observation
  -> EffectController policy decision
  -> MinimizeFeature or RestoreFeature transaction
  -> capture + AnimationConfiguration
  -> AnimationRun state transition
  -> OverlayWindow / AnimationRenderer
  -> shared completion or abort cleanup
```

`EffectPolicy` combines enabled state, temporary pause, fullscreen and
battery-saver suppression, power setting notifications (`RegisterPowerSettingNotification`),
and executable exclusions. Minimize and restore use explicit transaction contexts so
partially completed work restores native window state.

## Rendering and recovery

Animation and settings have separate rendering paths. `AnimationRenderer` and
`OverlayRenderer` render the captured mesh using HLSL shaders (`genie_mesh.hlsl`)
precompiled at build time via `fxc.exe` into C++ bytecode headers (`genie_vertex_shader.hpp`,
`genie_pixel_shader.hpp`). Runtime `D3DCompile` calls and `d3dcompiler_47.dll` dependencies are eliminated.

DXGI Desktop Duplication captures are maintained entirely within GPU VRAM (`D3D11_USAGE_DEFAULT`).
Subresource transfers use direct GPU-to-GPU `ID3D11DeviceContext::CopySubresourceRegion` without
CPU staging buffers (`Map`/`Unmap`). Display rotation angles (90°, 180°, 270°) pass rotation metadata
to shaders without CPU-side pixel transformations. Window mask textures use 8-bit single-channel
`DXGI_FORMAT_R8_UNORM` to save 75% VRAM compared to RGBA textures.

`ImguiRenderer` owns the settings swap chain, ImGui backends, fonts, DPI rebuilds, resize handling, and
device-loss recovery. Settings window dragging uses native Win32 `WM_NCHITTEST` returning `HTCAPTION`,
delegating window movement to the DWM compositor thread and enabling Windows 11 Snap Layouts.
DirectComposition visual tree interfaces (`IDCompositionDevice`, `Target`, `Visual`) use `wil::com_ptr`
for RAII lifetime management.

Desktop Duplication is isolated in `DesktopDuplicationSession`. Device removal
is surfaced to `RendererRecovery`, which applies bounded retry/backoff. Active
runs enter the shared abort/cleanup path before graphics resources are rebuilt.

## Windows boundary

The documented integration path uses WinEvent plus the separate CBT hook DLL,
per-window DWM transition suppression, temporary system-animation suppression,
DXGI Desktop Duplication, and a DirectComposition overlay.

`SingleInstanceGuard` owns the mutex and existing-instance activation.
`ProcessRuntime` owns DPI awareness, COM lifetime, and console-control shutdown
routing. Process elevation queries use `wil::get_token_information`. Registry startup
entries are safely managed via `wil::unique_hkey` and `wil::reg`. Log output is formatted via C++20 `std::format`
and dispatched via `spdlog`. `main.cpp` only coordinates these program-entry concerns.

`MINIMIZE_TASKBAR_RECT=left,top,right,bottom` overrides shell target discovery in
physical screen coordinates. Parsing and target selection remain isolated in
the taskbar provider.
