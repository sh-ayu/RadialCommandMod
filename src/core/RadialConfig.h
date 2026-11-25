#pragma once

#include "core/Localization.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace radial {

inline constexpr int    kMinSlotsPerMenu   = 2;
inline constexpr int    kMaxSlotsPerMenu   = 12;
inline constexpr int    kMaxSubmenuDepth   = 4;
inline constexpr int    kMaxMacroSteps     = 16;
inline constexpr int    kMaxPresets        = 64;
inline constexpr int    kMaxRulesPerPreset = 16;
inline constexpr size_t kMaxConfigFileSize = 2u * 1024u * 1024u; // 2 MiB guard

// Virtual key code constants (matches Win32 VK_* without pulling <windows.h> into core).
inline constexpr int kVkMButton   = 0x04;
inline constexpr int kVkXButton1  = 0x05;
inline constexpr int kVkXButton2  = 0x06;
inline constexpr int kVkBack      = 0x08;
inline constexpr int kVkTab       = 0x09;
inline constexpr int kVkReturn    = 0x0D;
inline constexpr int kVkShift     = 0x10;
inline constexpr int kVkControl   = 0x11;
inline constexpr int kVkMenu      = 0x12; // Alt (generic)
inline constexpr int kVkCapital   = 0x14; // CapsLock
inline constexpr int kVkEscape    = 0x1B;
inline constexpr int kVkSpace     = 0x20;
inline constexpr int kVkEnd       = 0x23;
inline constexpr int kVkHome      = 0x24;
inline constexpr int kVkLeft      = 0x25;
inline constexpr int kVkRight     = 0x27;
inline constexpr int kVkInsert    = 0x2D;
inline constexpr int kVkDelete    = 0x2E;
inline constexpr int kVkF1        = 0x70;
inline constexpr int kVkF6        = 0x75;
inline constexpr int kVkF9        = 0x78;
inline constexpr int kVkF12       = 0x7B;
inline constexpr int kVkLShift    = 0xA0;
inline constexpr int kVkRShift    = 0xA1;
inline constexpr int kVkLControl  = 0xA2;
inline constexpr int kVkRControl  = 0xA3;
inline constexpr int kVkLMenu     = 0xA4; // Left Alt
inline constexpr int kVkRMenu     = 0xA5; // Right Alt
inline constexpr int kVkOem3      = 0xC0; // Grave / Backquote

enum class ActivationMode {
    Hold,   // Hold key to keep wheel open; release to execute
    Toggle, // Tap key to open/close; left-click or tap again to execute
};

enum class ActionType {
    Command,       // Server command (e.g. "/hub")
    Chat,          // Normal chat message (e.g. "gg")
    ClientCommand, // LeviLamina client command (e.g. "/rc editor")
    Macro,         // Sequence of steps with optional per-step delay
    Submenu,       // Nested radial menu
};

struct MacroStep {
    ActionType  type    = ActionType::Command; // Command, Chat, or ClientCommand
    std::string value;
    int         delayMs = 0; // Delay BEFORE executing this step (0..10000)
};

struct RadialSlot {
    std::string             label;
    std::string             subtitle;
    ActionType              type       = ActionType::Command;
    std::string             value;      // Used by Command / Chat / ClientCommand
    std::string             color;      // Optional "#RRGGBB", empty = auto by ActionType
    int                     cooldownMs = -1; // -1 = use globalCooldownMs, >=0 = override (per-slot)
    std::vector<MacroStep>  steps;      // Used when type == ActionType::Macro
    std::vector<RadialSlot> children;   // Used when type == ActionType::Submenu
};

enum class ConnectionScope {
    Any,
    External,
    Local,
};

struct ServerRule {
    std::string     hostPattern;       // Wildcard match against unresolvedUrl or hostIp (e.g. "*.hivebedrock.network")
    int             port = 0;          // 0 = any port
    std::string     serverNamePattern; // Wildcard match against external server name (e.g. "*Hive*")
    std::string     worldNamePattern;  // Wildcard match against local world name (e.g. "*Creative*")
    ConnectionScope scope    = ConnectionScope::Any;
    int             priority = 0;      // Higher wins when multiple presets match
};

struct RadialPreset {
    std::string             id;
    std::string             name;
    std::vector<ServerRule> serverRules;
    std::vector<RadialSlot> slots;
};

struct RadialConfig {
    int            version             = 1;
    bool           enabled             = true;
    UiLanguage     language            = UiLanguage::Auto;
    ActivationMode activationMode      = ActivationMode::Hold;
    int            activationKey       = kVkMenu; // Default: Alt
    int            editorKey           = kVkF12;  // Default: F12 (0 = disabled)
    float          flickSensitivity    = 1.0f;    // Relative mouse flick sensitivity (0.2..4.0)
    float          outerRadius         = 92.0f;   // Ring outer radius in GUI px (50..180)
    float          innerRadius         = 32.0f;   // Ring inner radius in GUI px (14..85)
    float          deadzoneRadius      = 22.0f;   // Cancel deadzone radius in GUI px (8..80)
    float          backgroundOpacity   = 0.78f;   // Wheel panel opacity (0.15..1.0)
    float          fontScale           = 0.95f;   // Label text scale (0.5..1.8)
    int            holdDelayMs         = 0;       // Delay before wheel opens after press (0..500)
    int            submenuHoverMs      = 320;     // Auto-enter submenu on hover (0 = disabled)
    int            globalCooldownMs    = 350;     // Default cooldown between executions (0..10000)
    bool           executeOnRelease    = true;    // Execute selected slot when releasing activation key (Hold mode)
    bool           executeOnLeftClick  = true;    // Execute/enter selected slot on left click while wheel is open
    bool           autoSwitchPreset    = true;    // Automatically switch preset on server/world join
    bool           showPresetToast     = true;    // Show brief toast when preset changes or command runs
    bool           showNumberHints     = true;    // Show 1..9 preset switcher hints
    bool           invertWheelScroll   = false;   // Invert mouse wheel direction for preset switching
    bool           verboseLog          = false;
    std::string    defaultPresetId     = "default";
    std::vector<RadialPreset> presets;
};

struct ConfigLoadResult {
    RadialConfig config;
    bool         existed   = false;
    bool         corrupted = false;
    std::string  warning;
};

// --- Enum / key conversion helpers ---

std::string_view activationModeToString(ActivationMode m) noexcept;
std::optional<ActivationMode> parseActivationMode(std::string_view sv) noexcept;

std::string_view actionTypeToString(ActionType t) noexcept;
std::optional<ActionType> parseActionType(std::string_view sv) noexcept;

std::string_view connectionScopeToString(ConnectionScope s) noexcept;
std::optional<ConnectionScope> parseConnectionScope(std::string_view sv) noexcept;

// Symmetric VK matching: generic Alt/Shift/Ctrl match both left/right variants and vice versa.
bool keyMatchesBinding(int incomingVk, int bindingVk) noexcept;
bool isMouseButtonVk(int vk) noexcept;
std::string keyCodeToName(int vk);
std::optional<int> parseKeyName(std::string_view name) noexcept;

// Parse "#RRGGBB" into 0x00RRGGBB. Returns nullopt if invalid/empty.
std::optional<uint32_t> parseHexColor(std::string_view hex) noexcept;

// Build rich default configuration with Default, The Hive, CubeCraft, and Local World presets.
RadialConfig makeDefaultConfig();

// Clamp all numeric ranges and guarantee valid preset/slot structure (UTF-8 safe, unique preset IDs).
void sanitizeConfig(RadialConfig& cfg) noexcept;

// Parse JSON/JSONC text into RadialConfig (locale-independent, last-wins duplicate keys).
bool parseConfigJson(std::string_view jsonText, RadialConfig& outConfig, std::string* outWarning = nullptr);

// Serialize RadialConfig to clean pretty-printed JSON (locale-independent, finite-safe).
std::string serializeConfigJson(RadialConfig const& cfg);

// Load from disk (2 MiB guard). Missing file returns defaults (existed=false). Corrupt file is backed up.
ConfigLoadResult loadConfigFile(std::filesystem::path const& path);

// Crash-safe atomic write (.tmp + replace).
bool saveConfigFile(std::filesystem::path const& path, RadialConfig const& cfg);

// Find preset index by id or case-insensitive name.
int findPresetIndex(RadialConfig const& cfg, std::string_view idOrName) noexcept;

} // namespace radial
