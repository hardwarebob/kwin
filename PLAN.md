# Modular Per-Output Lock, DPMS & Remote Session Plan for KWin

Branch: `hermes-modular-lock` (based on `v6.8.3`)

## Goal
Support **two selectable modes** for remote-streaming / virtual-display workflows (e.g. Hermes, Sunshine, KRDP) in an upstream-friendly, modular way:

- **Mode A — Single-Session Per-Output Lock & DPMS (Shared Desktop):**
  1. Physical outputs (`HDMI-*`, `DP-*`) can show a local lock screen (`kscreenlocker`) and enter DPMS sleep (`DpmsMode::Off`) on idle.
  2. Designated virtual/remote outputs (e.g. `Virtual-1` / `HERMES-1`) remain **unlocked**, **awake (`DpmsMode::On`)**, and interactive in the **same** `kwin_wayland` session.
  3. Physical input stays confined to the lock screen while virtual/emulated input drives the unlocked virtual output without waking physical monitors.
- **Mode B — Independent / Isolated KWin Session (Separate `kwin_wayland` Instance):**
  1. Run a dedicated second `kwin_wayland` instance (`--no-lockscreen`, separate `WAYLAND_DISPLAY`) exclusively on the virtual KMS device (e.g. `KWIN_DRM_DEVICES=/dev/dri/card2`), leaving `seat0` (`tty1`) completely untouched so `seat0` can lock and DPMS-sleep normally.
  2. Both modes expose active remote session status to the `seat0` lock screen with **Unlock & Disconnect** or **Unlock & Keep Running**.

All abstractions are generic (no Hermes-specific logic in KWin) so they remain upstream-friendly.

---

## Architecture & Changes

### 1. Per-Output DPMS (`Workspace`, `BackendOutput`, `DpmsInterface`, `DpmsInputEventFilter`)
- **Current behavior:**
  - `Workspace::requestDpmsState()` (`src/workspace.cpp`) maintains a single global `Workspace::m_dpms` and applies it to all outputs via `applyOutputConfiguration()`.
  - `DpmsInterface` (`src/wayland/dpms.cpp`) forwards per-output Wayland DPMS requests to the global `workspace()->requestDpmsState()`.
  - `DpmsInputEventFilter` (`src/dpmsinputeventfilter.cpp`) wakes all outputs on any input event.
- **Planned changes:**
  - Track `DpmsMode` (`src/core/backendoutput.h`, `src/core/outputconfiguration.h`) per output rather than enforcing a single global state in `Workspace`.
  - Add an output DPMS policy (e.g. `FollowSession` vs `AlwaysOn` / `IgnoreIdleDpms`) so virtual streaming outputs stay `DpmsMode::On` while physical outputs can enter `DpmsMode::Off`.
  - Update `DpmsInputEventFilter` to wake only outputs matching the input device's affinity (so virtual input from a streamer does not wake sleeping physical monitors).

### 2. Per-Output Lock Policy & Scene Visibility (`WindowItem`, `WaylandServer`)
- **Current behavior:**
  - `WaylandServer::isScreenLocked()` (`src/wayland_server.cpp`) is session-global.
  - `WindowItem::computeVisibility()` (`src/scene/windowitem.cpp`) hides every non-lockscreen window whenever `isScreenLocked()` is true, and `WindowItem::updateVisibility()` suspends windows when `workspace()->dpmsState() != On`.
- **Planned changes:**
  - Add a per-output lock policy (`FollowSession` [default] vs `Unlocked` / `ExcludeFromLock`) configurable via `OutputConfiguration` / `org_kde_kwin_outputmanagement_v2` / output rules.
  - Update `WindowItem::computeVisibility()` and `WindowItem::updateVisibility()` so normal windows remain visible and unsuspended if they reside on at least one output that is both unlocked and `DpmsMode::On`.
  - Clip/mask normal window rendering on outputs that *are* locked so windows spanning outputs never leak onto a locked physical monitor; render `isLockScreen()`, `isInputMethod()`, and `isLockScreenOverlay()` surfaces only on locked outputs.

### 3. Input Device Affinity & Lock-Screen Routing Split (`InputDevice`, `LockScreenFilter`)
- **Current behavior:**
  - `LockScreenFilter` (`src/input.cpp`) intercepts all pointer, keyboard, and touch events when `waylandServer()->isScreenLocked()` is true, routing them only to lock-screen surfaces and resetting `KSldApp::userActivity()`.
- **Planned changes:**
  - Add an input affinity/scope attribute to `InputDevice` (`src/core/inputdevice.h`):
    - Physical `libinput` devices default to physical/session scope.
    - Virtual/emulated devices (`libei`/EIS, `FakeInput`, or udev-tagged virtual `uinput` devices) can be scoped to `Unlocked` / specific virtual outputs.
  - Update `LockScreenFilter` so events from virtual-scoped devices pass through to normal windows on unlocked outputs (and do not trigger `KSldApp::userActivity()` on the physical lock screen), while physical input stays strictly confined to the lock screen.

### 4. Remote Session Registry & Lock Screen Controls (`org.kde.KWin.RemoteSessions` + `kscreenlocker` Greeter)
- **KWin D-Bus interface (`org.kde.KWin.RemoteSessions`):**
  - Generic registration API for remote-display providers (Hermes, KRDP, Sunshine, etc.):
    - `RegisterSession(provider, clientName, outputName) -> sessionId`
    - `UnregisterSession(sessionId)`
    - Property `ActiveSessions` + `ActiveSessionsChanged` signal
    - Signal `DisconnectRequested(sessionId)` (emitted only after authenticated local unlock action)
- **`kscreenlocker` / Plasma Greeter UI (companion change outside KWin):**
  - Reads `ActiveSessions` from `org.kde.KWin.RemoteSessions` and displays an indicator banner (e.g., *"Active remote session: Hermes (tablet)"*).
  - Provides two authenticated unlock actions:
    1. **Unlock & Disconnect Remote Session** — unlocks `seat0`, calls `RequestDisconnect(sessionId)` so the provider tears down the stream and virtual output, and restores physical monitors.
    2. **Unlock & Keep Running** — unlocks `seat0` and restores physical monitors while leaving the remote session and virtual output active.

### 5. Independent Session Support (`DrmBackend`, `Session`, Virtual-Only Input) — *Mode B*
- **Current behavior:**
  - `DrmBackend` (`src/backends/drm/`) requires a `logind`/`libseat` seat session (`seat0`) and skips DRM devices whose `seat()` does not match `session->seat()`.
  - A display-only KMS device like `hermes-kms` (`/dev/dri/card2`) has no 3D render engine of its own and needs a separate GPU render node (e.g. `/dev/dri/renderD128`) for GBM/EGL compositing without taking DRM master on `card0`.
  - `libinput` opens all physical `seat0` input devices by default.
- **Planned changes:**
  - **Seatless / non-VT DRM session mode:** Allow `kwin_wayland` (via a CLI flag / env var such as `--drm-standalone` or `Session::Type::Noop` paired with `KWIN_DRM_DEVICES`) to take DRM master on explicitly specified non-seat0 DRM devices (e.g. `/dev/dri/card2` tagged off `seat0` via udev) without claiming `seat0` VT ownership, so it stays active when `tty1` is locked or sleeping.
  - **Explicit render-node pairing:** Allow `DrmBackend` to use an explicit render node (e.g. `KWIN_DRM_RENDER_NODE=/dev/dri/renderD128`) for GBM/EGL compositing while scanning out to the display-only KMS device (`card2`), without opening `card0` as DRM master.
  - **Virtual-only input mode:** Add an option (e.g. `--no-libinput` or `InputBackend` filter) so the independent `kwin_wayland` instance skips physical `seat0` `libinput` devices and accepts input exclusively via `libei`/EIS or `FakeInput`.

---

## Implementation Order
1. **Per-output DPMS** (`Workspace`, `DpmsInterface`, `DpmsInputEventFilter`) — *Mode A*
2. **Per-output lock policy & `WindowItem` visibility/clipping** — *Mode A*
3. **`InputDevice` affinity & `LockScreenFilter` routing split** — *Mode A*
4. **Independent session support** (seatless `DrmBackend`, explicit render node, virtual-only input) — *Mode B*
5. **`org.kde.KWin.RemoteSessions` D-Bus interface** (+ companion `kscreenlocker` greeter UI & Hermes registration across both Mode A and Mode B)
