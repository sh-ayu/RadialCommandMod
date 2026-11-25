#pragma once

#include "core/Localization.h"
#include "core/RadialConfig.h"
#include "core/ServerMatcher.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace radial {

struct QueuedAction {
    ActionType  type = ActionType::Command; // Command, Chat, or ClientCommand
    std::string value;
    std::string sourceLabel;
    int64_t     executeAtMs = 0;
};

struct ToastState {
    std::string text;
    float       alpha = 0.0f;
};

struct RadialViewSnapshot {
    bool                      open              = false;
    float                     openProgress      = 0.0f; // 0..1 smooth open animation
    float                     pointerX          = 0.0f; // Virtual pointer offset from center (GUI px)
    float                     pointerY          = 0.0f;
    int                       hoveredIndex      = -1;   // -1 when in deadzone
    float                     submenuHoverRatio = 0.0f; // 0..1 progress toward auto-entering submenu
    int                       activePresetIndex = 0;
    int                       presetCount       = 0;
    std::string               presetName;
    std::string               breadcrumb;               // e.g. "The Hive > Quick Chat"
    bool                      inSubmenu         = false;
    bool                      manualOverride    = false;
    std::string               serverDisplay;
    std::vector<RadialSlot>   visibleSlots;
    std::optional<ToastState> toast;
};

class RadialController {
public:
    RadialController();

    // Update configuration and re-evaluate active preset.
    void setConfig(RadialConfig const& cfg, int64_t nowMs);

    // Notify server/world connection info update. Automatically switches preset when matched preset changes.
    // Returns true if active preset actually changed.
    bool updateServerIdentity(ServerIdentity const& server, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // Manually select preset by index (0..presets.size()-1). Pass -1 to restore auto-match.
    bool selectPreset(int presetIndex, RadialConfig const& cfg, int64_t nowMs, bool isManual = true, ResolvedLang lang = ResolvedLang::En);
    bool cyclePreset(int delta, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // --- Wheel lifecycle (supports Hold & Toggle modes) ---

    // Activation key/button pressed.
    void onActivationDown(RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // Activation key/button released. In Hold mode, may enqueue selected action if executeOnRelease is true.
    bool onActivationUp(RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // Force-cancel wheel without executing (e.g. Esc, window focus lost, menu opened).
    void cancelWheel();

    // Accumulate relative mouse motion while wheel is open.
    void onMouseDelta(float dx, float dy, RadialConfig const& cfg, int64_t nowMs);

    // Left click while wheel is open: enter submenu or execute immediately if configured.
    bool onLeftClick(RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // Right click or Backspace while wheel is open: go back to parent menu, or cancel if at root.
    void onBackOrCancel(int64_t nowMs);

    // Mouse wheel scroll while wheel is open: switch preset.
    bool onMouseWheel(int delta, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // Number key 1..9 while wheel is open: jump directly to preset index (0..8).
    bool onNumberKey(int digit1To9, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // Enqueue a single slot directly (used by editor "Test Run" button).
    bool enqueueSlotForTest(RadialSlot const& slot, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang = ResolvedLang::En);

    // Advance timers (holdDelayMs arming, submenuHoverMs auto-enter, animations).
    void update(RadialConfig const& cfg, int64_t nowMs);

    // Pop all queued actions whose executeAtMs <= nowMs.
    std::vector<QueuedAction> pollDueActions(int64_t nowMs);

    // Clear all pending macro steps (e.g. on world exit).
    void clearPendingActions();

    // Trigger a short status toast.
    void showToast(std::string text, int64_t nowMs, int durationMs = 1800);

    // Build thread-safe rendering snapshot.
    RadialViewSnapshot buildSnapshot(RadialConfig const& cfg, int64_t nowMs) const;

    // Inspection helpers.
    bool isArming() const noexcept { return mArming; }
    bool isWheelOpen() const noexcept { return mOpen; }
    // Only capture mouse movement when the wheel is actually open (arming never freezes the camera!).
    bool isCapturingInput() const noexcept { return mOpen; }
    bool isPressTracked() const noexcept { return mOpen || mArming; }
    int  activePresetIndex() const noexcept { return mActivePresetIndex; }
    int  hoveredIndex() const noexcept { return mHoveredIndex; }
    bool isManualOverride() const noexcept { return mManualOverride; }
    ServerIdentity const& currentServer() const noexcept { return mServer; }
    size_t pendingActionCount() const noexcept { return mPendingActions.size(); }

private:
    std::vector<RadialSlot> const* resolveCurrentSlots(RadialConfig const& cfg) const noexcept;
    std::string buildBreadcrumb(RadialConfig const& cfg) const;
    std::string buildSlotCooldownKey(int slotIndex) const;
    void recomputeHover(RadialConfig const& cfg, int64_t nowMs);
    bool executeSlot(RadialSlot const& slot, std::string const& slotKey, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang);
    bool enterSubmenu(int slotIdx, RadialConfig const& cfg, int64_t nowMs);
    void clampPresetAndPath(RadialConfig const& cfg);

    static constexpr size_t kMaxPendingActions      = 32;
    static constexpr int64_t kSubmenuGraceMs        = 120;
    static constexpr float   kAngleHysteresisRad    = 0.065f; // ~3.7 deg prevents boundary flicker
    static constexpr float   kDeadzoneExitFactor    = 0.82f;  // Schmitt trigger for leaving/entering center

    bool    mArming           = false;
    bool    mOpen             = false;
    int64_t mPressStartedMs   = 0;
    int64_t mOpenedAtMs       = 0;
    int64_t mSubmenuGraceEndMs = 0;
    float   mPointerX         = 0.0f;
    float   mPointerY         = 0.0f;
    int     mHoveredIndex     = -1;
    int64_t mHoverSinceMs     = 0;

    int              mActivePresetIndex = 0;
    bool             mManualOverride    = false;
    std::string      mLastAutoMatchedPresetId;
    std::vector<int> mSubmenuPath;

    ServerIdentity mServer;
    std::string    mLastServerFingerprint;

    int64_t                                  mLastGlobalExecMs = -1000000;
    std::unordered_map<std::string, int64_t> mSlotLastExecMs;
    std::vector<QueuedAction>                mPendingActions;

    std::string mToastText;
    int64_t     mToastUntilMs = 0;
};

} // namespace radial
