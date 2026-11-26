# RadialCommandMod

[日本語版 README はこちら (README_JA.md)](README_JA.md)

A **LeviLamina client native mod** for Minecraft Bedrock (Windows x64) that adds a hold-or-toggle **radial command & quick-chat wheel** with instant relative flick selection, automatic per-server preset switching, nested submenus, timed multi-step macros, dynamic placeholders, and a bilingual in-game visual GUI editor.

> **Compatibility & Verification**: Built for **LeviLamina 26.60.8** (Minecraft Bedrock **1.26.60.5**, Windows x64). Verified with the standalone C++20 unit test suite (`radial_tests`) and in-game testing on LeviLauncher.

---

## Key Features

- **Camera-Locked Relative Flick Wheel (Hold & Toggle Modes)**
  - Open with `Alt` (configurable to `Tab`, `R`, `V`, `G`, `CapsLock`, `Mouse4`, `Mouse5`, `MouseMiddle`, etc.) using OS-level physical key polling so Windows `Alt` (`WM_SYSKEYDOWN`) works reliably.
  - Absorbs mouse movement while open so your camera never drifts. Includes smooth per-sector hover animation, angular & deadzone hysteresis, and deadzone ring visualization.
  - Rendered in a **single GPU draw call** via Bedrock's native UI triangle-mesh tessellator for crisp, gap-free circular sectors and minimal frame-time impact.
  - Supports both **Hold** and **Toggle** activation modes.
- **Automatic Per-Server Preset Switching**
  - Inspects `ClientInstance::getGameConnectionInfo()` (domain, IP, port, external server name), Realms status, and local world name.
  - Lexicographic `(priority, specificity)` wildcard matching (`*`, `?`) with ready-to-use **Default**, **The Hive**, **CubeCraft**, and **Local World** presets.
- **Instant Manual Preset Switching**
  - Scroll mouse wheel or press `1`–`9` while the wheel is open (2–12 slots per wheel).
- **Submenus, Timed Macros, Independent Cooldowns & Placeholders**
  - **Nested Submenus (up to 4 levels)**: Open via left-click or steady hover (`320ms` default) with inertia grace protection; right-click / `Backspace` / center-click to step back.
  - **Multi-Step Macros (up to 16 steps)**: Chain chat and commands with per-step `delayMs` (e.g., send `gg`, then `/hub` after `250ms`).
  - **Independent Cooldowns**: Global spam guard and per-slot cooldown overrides tracked separately.
  - **Dynamic Placeholders**: `{player}`, `{x}`, `{y}`, `{z}`, `{server}`, `{world}` expand live on execution.
- **Intuitive 3-Tab In-Game Visual GUI Editor (`F12` or `/rc editor`)**
  - **1. Wheel**: Interactive circular wheel preview (click sectors to edit, re-click submenus to drill down), **16-color accent palette** (2×8 swatches), live opacity/size/color preview, and a **Test Run** button.
  - **2. Servers**: Multi-rule server editor with priority controls, port field, one-click **`+ Create for Current Server`**, **`Current`** auto-fill buttons, and a live **`MATCH / NO MATCH`** badge.
  - **3. Settings**: Key capture (including `Alt` and side mouse buttons), draggable tuning sliders, ON/OFF toggle switches, and default reset.
  - **Refined Editing UX & Bilingual EN / JA UI**: `auto` / `en` / `ja` with one-click `[JA/EN]` header toggle, dynamic 1-line footer hover hints, blinking cyan caret bar with click-to-caret, double-click select-all, horizontal text scrolling, `Tab` / `Shift+Tab` field cycling, `Ctrl+Z` Undo, unsaved-changes guard, non-destructive pause / window-switch persistence, OS keyboard layout translation (`ToUnicodeEx` for US/JIS/international layouts), and `Ctrl+A/C/V` UTF-8 clipboard support.

---

## Installation

### 1. Via `lip` (Recommended)

```bat
lip install github.com/sh-ayu/RadialCommandMod
```

Both `lip.toml` (for LeviMC Mods / LeviLauncher) and `tooth.json` are included, and your `mods/RadialCommandMod/config/` directory is automatically preserved across upgrades.

### 2. Manual Release ZIP

1. Download `RadialCommandMod-client-windows-x64.zip` from [GitHub Releases](https://github.com/sh-ayu/RadialCommandMod/releases) and extract it.
2. Place the `RadialCommandMod` folder inside your LeviLauncher instance's `mods/` directory:
   ```text
   %LOCALAPPDATA%\LeviLauncher\Instances\<InstanceName>\mods\RadialCommandMod\
   ├── RadialCommandMod.dll
   ├── manifest.json
   ├── config.sample.json
   ├── CHANGELOG.md
   ├── LICENSE
   ├── README.md
   └── README_JA.md
   ```
3. Launch Minecraft via LeviLauncher. `mods/RadialCommandMod/config/config.json` is created automatically on first run.

---

## Controls

| Input | Action |
|---|---|
| Hold `Alt` (default) | Open radial wheel (tap to open/close in Toggle mode) |
| Flick mouse + release key | Execute highlighted sector (Hold mode) |
| Left click | Enter submenu or execute highlighted sector immediately |
| Right click / `Backspace` | Return to parent menu (or cancel at root) |
| `Esc` / release in center | Cancel and close |
| Mouse wheel / `1`–`9` | Switch active preset manually |
| `F12` or `/rc editor` | Open in-game visual GUI editor |

---

## Client Commands (`/rc`)

| Command | Description |
|---|---|
| `/rc` or `/rc help` | Show command list |
| `/rc status` | Show enabled state, mode, keybinds, active preset, and config path |
| `/rc editor` | Open the in-game visual GUI editor |
| `/rc server` | Show detected server/world info and auto-match result |
| `/rc presets` | List all configured presets |
| `/rc preset <id\|name\|auto>` | Switch preset manually or restore automatic server matching |
| `/rc bind <key>` | Change wheel key (`Alt`, `Tab`, `R`, `V`, `G`, `Mouse4`, `Mouse5`, etc.) |
| `/rc editorkey <key\|None>` | Change editor hotkey (default `F12`, `None` to disable) |
| `/rc mode <hold\|toggle>` | Switch between `hold` and `toggle` modes |
| `/rc lang <auto\|ja\|en>` | Switch UI language |
| `/rc sensitivity <0.2..4.0>` | Adjust relative flick sensitivity |
| `/rc cooldown <ms>` | Set global cooldown in milliseconds |
| `/rc toggle` | Toggle mod ON / OFF |
| `/rc reload` / `/rc reset` | Reload `config.json` / reset to defaults |

---

## Configuration (`config.json`)

Supports JSON and JSONC (`//` comments, `/* ... */` block comments, trailing commas). Out-of-range values are clamped automatically, and corrupt files are backed up to `config.json.corrupt-<timestamp>` before regenerating defaults.

| Key | Default | Range / Values | Description |
|---|---|---|---|
| `enabled` | `true` | `true` / `false` | Master enable switch |
| `language` | `"auto"` | `"auto"`, `"ja"`, `"en"` | UI language (`auto` uses OS UI language) |
| `activationMode` | `"hold"` | `"hold"`, `"toggle"` | Hold-to-open or tap-to-toggle |
| `activationKey` | `"Alt"` | Key name or VK int | Wheel activation key |
| `editorKey` | `"F12"` | Key name or `"None"` | Visual editor hotkey |
| `flickSensitivity` | `1.00` | `0.20`..`4.00` | Relative mouse flick sensitivity |
| `outerRadius` | `92.00` | `50.0`..`180.0` | Wheel outer radius (GUI px) |
| `innerRadius` | `32.00` | `14.0`..`85.0` | Wheel inner hub radius (GUI px) |
| `deadzoneRadius` | `22.00` | `8.0`..`80.0` | Center cancel deadzone radius (GUI px) |
| `backgroundOpacity` | `0.78` | `0.15`..`1.00` | Wheel background opacity |
| `fontScale` | `0.95` | `0.50`..`1.80` | Sector label text scale |
| `holdDelayMs` | `0` | `0`..`500` | Hold delay before opening wheel (ms) |
| `submenuHoverMs` | `320` | `0`..`1500` | Hover time to auto-enter submenu (`0` = off) |
| `globalCooldownMs` | `350` | `0`..`10000` | Global spam-prevention cooldown (ms) |
| `executeOnRelease` | `true` | `true` / `false` | Execute selected slot on key release (Hold mode) |
| `executeOnLeftClick` | `true` | `true` / `false` | Execute/open selected slot on left click |
| `autoSwitchPreset` | `true` | `true` / `false` | Auto-switch presets on server/world join |
| `showPresetToast` | `true` | `true` / `false` | Show top toast on preset switch / execution |
| `showNumberHints` | `true` | `true` / `false` | Show `1`–`9` preset hint in footer |
| `invertWheelScroll` | `false` | `true` / `false` | Invert mouse wheel preset cycling direction |
| `verboseLog` | `false` | `true` / `false` | Enable verbose logging |
| `defaultPresetId` | `"default"` | Preset ID | Fallback preset when no rule matches |

### Dynamic Placeholders

Available in command, chat, and macro step `value` strings:
- `{player}` — Local player name
- `{x}`, `{y}`, `{z}` — Integer player coordinates
- `{server}` — Current server display label
- `{world}` — Current world name

---

## Safety & Limitations

- Client-side utility only: sends standard `TextPacket` / `CommandRequestPacket` strictly upon explicit user interaction. Automatically gated off during chat, pause, inventory, server form, death, and bed screens.
- **IME Composition**: The overlay editor uses OS keyboard layout translation (`ToUnicodeEx`) for direct typing and supports UTF-8 clipboard paste (`Ctrl+V`) for Japanese/CJK text.
- **No Live Unload**: Restart Minecraft when updating or removing the mod DLL.

---

## Building from Source

Requires Windows x64, Visual Studio Build Tools, LLVM (`clang-cl`), and [xmake](https://xmake.io/) 3.0+.

```bat
:: 1. Run standalone unit tests (no LeviLamina needed)
xmake f -m release --with_mod=n -y
xmake build radial_tests
xmake run radial_tests

:: 2. Build LeviLamina 26.60.8 client mod DLL
xmake f -m release --with_mod=y -y
xmake build RadialCommandMod

:: 3. Package release ZIP (dist/RadialCommandMod-client-windows-x64.zip)
powershell -ExecutionPolicy Bypass -File scripts/package.ps1
```

## License

[MIT License](LICENSE)
