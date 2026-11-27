# Changelog

All notable changes to **RadialCommandMod** are documented in this file.

## [1.0.2] - 2026-11-27

### Fixed
- **Toggle mode no longer double-fires**: The wheel only reacts to a real key-down edge, so UI polling, key events, and OS key repeat cannot toggle it twice. Activation key state is now guarded by the same lock as the wheel controller.
- **Hidden HUD (F1)**: The wheel cannot open or draw while the HUD is hidden, and an open wheel is cancelled.
- **Server rules match more host formats**: Hosts with a scheme (`https://`), path, `user@`, `:port` suffix, or bracketed IPv6 (`[::1]:19132`) are normalized before matching, and an embedded port is used when the connection reports none.
- **Strict number parsing**: Port fields, key names (`F1`-`F12`, `vk<code>`), editor IDs, and the `flickSensitivity` / `globalCooldownMs` commands reject malformed numbers instead of silently reading `0`; the commands report an invalid value.
- **Editor**: Settings toggle/slider pages are clamped to the real page count, new rules use the current connection's local/external scope, and host/name pattern fields accept up to 128 characters.
- **Rendering**: The shader color is restored even on early return.

### Changed
- Add a mod icon (`assets/icon.png`) and use it as the package logo on LeviMC Mods instead of the author's avatar.

## [1.0.1] - 2026-11-26

### Fixed
- **Allow `Shift+Alt` while sneaking**: Removed `Shift` from the Windows Alt-chord suppression list so players can open the radial wheel while holding sneak (`Shift`).
- **Tightened Alt-chord suppression**: Only suppress the wheel on Alt system chords (`Alt+Tab`, `Alt+Enter`, `Alt+Space`, `Alt+F4`, `Alt+Esc`) while the activation key is physically held, and guard early in `onKeyInput` as well.

### Packaging & Metadata
- Added `lip.toml` (format 4) with official LeviMC Mods category tags (`utility`, `visual`), logo, and config directory preservation (`mods/RadialCommandMod/config/`).
- Updated `scripts/package.ps1` to validate `lip.toml` version parity alongside `manifest.json`, `tooth.json`, and `xmake.lua`.

## [1.0.0] - 2026-11-26

### Target & Verification
- Built for **LeviLamina 26.60.8** (Minecraft Bedrock **1.26.60.5**, Windows x64 client).
- Verified with standalone C++20 unit test suite (`radial_tests`, 473 checks) and in-game testing on LeviLauncher.

### Added
- **Relative Flick Radial Wheel (Hold & Toggle Modes)**:
  - Open with `Alt` (configurable to `Tab`, `R`, `V`, `G`, `CapsLock`, `Mouse4`, `Mouse5`, `MouseMiddle`, etc.) with OS-level physical key polling so Windows `Alt` (`WM_SYSKEYDOWN`) works reliably.
  - Complete camera lock while open, smooth per-sector hover animation, angular & deadzone hysteresis, deadzone ring visualization, and screen-clamped responsive rendering.
  - **Single-draw-call GPU mesh renderer** via Bedrock's native UI `Tessellator` (`ui_fill_gradient`) for crisp, gap-free sectors with minimal frame-time impact (shared by the in-game wheel and editor preview).
- **Per-Server Automatic Preset Switching**:
  - Detects external domains, IPs, ports, server names, Realms, and local world names via `ClientInstance::getGameConnectionInfo()` and `LevelData`.
  - Lexicographic `(priority, specificity)` wildcard matching (`*`, `?`) with ready-to-use **Default**, **The Hive**, **CubeCraft**, and **Local World** presets.
- **Nested Submenus, Timed Macros, Cooldowns & Dynamic Placeholders**:
  - Up to 4 levels of nested submenus (enter via left-click or steady hover with inertia grace period).
  - Multi-step macros (up to 16 steps with per-step `delayMs`), independent global & per-slot cooldowns.
  - Live placeholder expansion: `{player}`, `{x}`, `{y}`, `{z}`, `{server}`, `{world}`.
- **In-Game Visual GUI Editor (`F12` or `/rc editor`)**:
  - 3-tab responsive overlay with an interactive circular wheel preview, **16-color accent palette** (2×8 swatches), draggable tuning sliders, multi-rule server editor with real-time match badge, one-click `+ Create for Current Server`, Undo (`Ctrl+Z`), Test Run, and unsaved-changes guard.
  - Refined text editing and UX: blinking cyan caret bar with click-to-caret, double-click select-all, horizontal text scrolling, `Tab` / `Shift+Tab` field cycling, dynamic 1-line footer hover hints, non-destructive pause & window-switch persistence, JIS/US keyboard layout support (`ToUnicodeEx`), and `Ctrl+A/C/V` UTF-8 clipboard support.
  - Bilingual Japanese / English UI (`auto` / `ja` / `en`) with one-click `[JA/EN]` toggle.
- **Client Commands (`/rc`) & Packaging**:
  - Full `/rc` command suite (`status`, `editor`, `server`, `presets`, `preset`, `bind`, `editorkey`, `mode`, `lang`, `sensitivity`, `cooldown`, `toggle`, `reload`, `reset`).
  - `lip` manifest (`tooth.json`) with POSIX-path release ZIP packaging, `preserve_files` protection for `config/config.json`, and stripped local PDB paths (`/PDBALTPATH:%_PDB%`).
