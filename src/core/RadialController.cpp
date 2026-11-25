#include "core/RadialController.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace radial {
namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

float wrapAnglePi(float a) noexcept {
    a = std::fmod(a + kPi, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a - kPi;
}

float sectorCenterAngle(int idx, int count) noexcept {
    float span = kTwoPi / static_cast<float>(count);
    return -kPi * 0.5f + span * static_cast<float>(idx);
}

int sectorIndexFromVector(float x, float y, int count) noexcept {
    if (count <= 0) return -1;
    float angle      = std::atan2(y, x);
    float sectorSpan = kTwoPi / static_cast<float>(count);
    float shifted    = angle + (kPi * 0.5f) + (sectorSpan * 0.5f);
    shifted          = std::fmod(shifted, kTwoPi);
    if (shifted < 0.0f) shifted += kTwoPi;
    int idx = static_cast<int>(std::floor(shifted / sectorSpan));
    return std::clamp(idx, 0, count - 1);
}

bool isBlankString(std::string_view sv) noexcept {
    for (unsigned char c : sv) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

} // namespace

RadialController::RadialController() = default;

void RadialController::setConfig(RadialConfig const& cfg, int64_t nowMs) {
    clampPresetAndPath(cfg);
    if (cfg.autoSwitchPreset && !mManualOverride && mServer.inWorld) {
        auto match               = matchPresetForServer(cfg, mServer);
        mActivePresetIndex       = std::max(0, match.presetIndex);
        mLastAutoMatchedPresetId = match.matchedPresetId;
        mSubmenuPath.clear();
    }
    if (mOpen) {
        recomputeHover(cfg, nowMs);
    }
}

bool RadialController::updateServerIdentity(
    ServerIdentity const& server,
    RadialConfig const&   cfg,
    int64_t               nowMs,
    ResolvedLang          lang
) {
    bool const wasInWorld = mServer.inWorld;
    mServer               = server;
    std::string fp        = server.fingerprint();
    if (fp == mLastServerFingerprint) {
        return false;
    }
    mLastServerFingerprint = fp;

    if (!server.inWorld) {
        cancelWheel();
        clearPendingActions();
        mManualOverride          = false;
        mLastAutoMatchedPresetId.clear();
        int  defIdx              = std::max(0, findPresetIndex(cfg, cfg.defaultPresetId));
        bool changed             = (defIdx != mActivePresetIndex);
        mActivePresetIndex       = defIdx;
        return changed;
    }

    if (!cfg.autoSwitchPreset) {
        clampPresetAndPath(cfg);
        return false;
    }

    auto match = matchPresetForServer(cfg, server);
    int  next  = std::max(0, match.presetIndex);

    // If transferring between sub-servers of the same network (e.g. Hive lobby <-> game),
    // the auto-matched preset ID stays identical: preserve manual override and avoid spamming toasts!
    bool const sameNetworkGroup = wasInWorld && !mLastAutoMatchedPresetId.empty()
                               && (match.matchedPresetId == mLastAutoMatchedPresetId);
    mLastAutoMatchedPresetId = match.matchedPresetId;

    if (sameNetworkGroup && mManualOverride) {
        return false;
    }

    mManualOverride = false;
    mSubmenuPath.clear();
    bool changed       = (next != mActivePresetIndex);
    mActivePresetIndex = next;
    clampPresetAndPath(cfg);

    if (cfg.showPresetToast && !cfg.presets.empty() && (!wasInWorld || changed)) {
        auto const& preset = cfg.presets[static_cast<size_t>(mActivePresetIndex)];
        std::string msg    = std::string(tr(lang, "hud.toast.preset")) + preset.name;
        if (match.matchedRule) msg += tr(lang, "hud.toast.auto");
        showToast(std::move(msg), nowMs, 2000);
    }
    return changed;
}

bool RadialController::selectPreset(
    int                 presetIndex,
    RadialConfig const& cfg,
    int64_t             nowMs,
    bool                isManual,
    ResolvedLang        lang
) {
    if (cfg.presets.empty()) return false;
    int target = presetIndex;
    if (target < 0) {
        mManualOverride = false;
        auto match      = matchPresetForServer(cfg, mServer);
        target          = std::max(0, match.presetIndex);
    } else {
        if (target >= static_cast<int>(cfg.presets.size())) return false;
        mManualOverride = isManual;
    }

    mActivePresetIndex = target;
    mSubmenuPath.clear();
    mPointerX          = 0.0f;
    mPointerY          = 0.0f;
    mHoveredIndex      = -1;
    mHoverSinceMs      = nowMs;

    if (cfg.showPresetToast) {
        std::string msg = std::string(tr(lang, "hud.toast.preset"))
                        + cfg.presets[static_cast<size_t>(mActivePresetIndex)].name;
        if (!mManualOverride) msg += tr(lang, "hud.toast.auto");
        showToast(std::move(msg), nowMs, 1500);
    }
    return true;
}

bool RadialController::cyclePreset(int delta, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang) {
    if (cfg.presets.size() <= 1 || delta == 0) return false;
    int n    = static_cast<int>(cfg.presets.size());
    int next = (mActivePresetIndex + (delta % n) + n) % n;
    return selectPreset(next, cfg, nowMs, true, lang);
}

void RadialController::onActivationDown(RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang) {
    if (!cfg.enabled || cfg.presets.empty()) return;

    // Toggle mode: pressing again while open executes hovered item (or closes if in deadzone).
    if (cfg.activationMode == ActivationMode::Toggle) {
        if (mOpen) {
            auto const* slots = resolveCurrentSlots(cfg);
            if (slots && mHoveredIndex >= 0 && mHoveredIndex < static_cast<int>(slots->size())) {
                auto const& slot = (*slots)[static_cast<size_t>(mHoveredIndex)];
                if (slot.type == ActionType::Submenu) {
                    enterSubmenu(mHoveredIndex, cfg, nowMs);
                    return;
                }
                executeSlot(slot, buildSlotCooldownKey(mHoveredIndex), cfg, nowMs, lang);
            }
            cancelWheel();
            return;
        }
        clampPresetAndPath(cfg);
        mSubmenuPath.clear();
        mPointerX          = 0.0f;
        mPointerY          = 0.0f;
        mHoveredIndex      = -1;
        mHoverSinceMs      = nowMs;
        mArming            = false;
        mOpen              = true;
        mOpenedAtMs        = nowMs;
        mSubmenuGraceEndMs = nowMs;
        return;
    }

    // Hold mode
    if (mOpen || mArming) return;

    clampPresetAndPath(cfg);
    mSubmenuPath.clear();
    mPointerX          = 0.0f;
    mPointerY          = 0.0f;
    mHoveredIndex      = -1;
    mPressStartedMs    = nowMs;
    mHoverSinceMs      = nowMs;
    mSubmenuGraceEndMs = nowMs;

    if (cfg.holdDelayMs <= 0) {
        mArming     = false;
        mOpen       = true;
        mOpenedAtMs = nowMs;
    } else {
        mArming = true;
        mOpen   = false;
    }
}

bool RadialController::onActivationUp(RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang) {
    // In Toggle mode, releasing the activation key keeps the wheel open.
    if (cfg.activationMode == ActivationMode::Toggle) {
        return false;
    }

    bool wasOpen = mOpen;
    mArming      = false;
    if (!wasOpen) {
        return false;
    }

    auto const* slots = resolveCurrentSlots(cfg);
    int         sel   = mHoveredIndex;
    bool        acted = false;

    if (cfg.executeOnRelease && slots && sel >= 0 && sel < static_cast<int>(slots->size())) {
        auto const& slot = (*slots)[static_cast<size_t>(sel)];
        if (slot.type != ActionType::Submenu) {
            acted = executeSlot(slot, buildSlotCooldownKey(sel), cfg, nowMs, lang);
        }
    }

    cancelWheel();
    return acted;
}

void RadialController::cancelWheel() {
    mArming            = false;
    mOpen              = false;
    mPointerX          = 0.0f;
    mPointerY          = 0.0f;
    mHoveredIndex      = -1;
    mSubmenuGraceEndMs = 0;
    mSubmenuPath.clear();
}

void RadialController::onMouseDelta(float dx, float dy, RadialConfig const& cfg, int64_t nowMs) {
    if (!mOpen) return;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    // Brief inertia grace right after entering a submenu prevents accidental instant selection.
    if (nowMs < mSubmenuGraceEndMs) return;

    float scale = 0.55f * cfg.flickSensitivity;
    mPointerX += dx * scale;
    mPointerY += dy * scale;

    float maxR = std::max(cfg.outerRadius, cfg.deadzoneRadius + 16.0f);
    float dist = std::sqrt(mPointerX * mPointerX + mPointerY * mPointerY);
    if (dist > maxR && dist > 1e-4f) {
        float k = maxR / dist;
        mPointerX *= k;
        mPointerY *= k;
    }

    recomputeHover(cfg, nowMs);
}

bool RadialController::onLeftClick(RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang) {
    if (!mOpen) return false;
    auto const* slots = resolveCurrentSlots(cfg);
    if (!slots) return false;

    if (mHoveredIndex < 0) {
        if (!mSubmenuPath.empty()) {
            onBackOrCancel(nowMs);
            return true;
        }
        return false;
    }

    if (mHoveredIndex >= static_cast<int>(slots->size())) return false;
    auto const& slot = (*slots)[static_cast<size_t>(mHoveredIndex)];

    if (slot.type == ActionType::Submenu) {
        return enterSubmenu(mHoveredIndex, cfg, nowMs);
    }

    if (cfg.executeOnLeftClick || cfg.activationMode == ActivationMode::Toggle) {
        bool ok = executeSlot(slot, buildSlotCooldownKey(mHoveredIndex), cfg, nowMs, lang);
        if (ok) cancelWheel();
        return ok;
    }
    return false;
}

void RadialController::onBackOrCancel(int64_t nowMs) {
    if (!mOpen) {
        mArming = false;
        return;
    }
    if (!mSubmenuPath.empty()) {
        mSubmenuPath.pop_back();
        mPointerX          = 0.0f;
        mPointerY          = 0.0f;
        mHoveredIndex      = -1;
        mHoverSinceMs      = nowMs;
        mOpenedAtMs        = nowMs;
        mSubmenuGraceEndMs = nowMs + kSubmenuGraceMs;
        return;
    }
    cancelWheel();
}

bool RadialController::onMouseWheel(int delta, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang) {
    if (!mOpen || delta == 0) return false;
    int step = (delta > 0) ? -1 : 1;
    if (cfg.invertWheelScroll) step = -step;
    return cyclePreset(step, cfg, nowMs, lang);
}

bool RadialController::onNumberKey(int digit1To9, RadialConfig const& cfg, int64_t nowMs, ResolvedLang lang) {
    if (!mOpen || digit1To9 < 1 || digit1To9 > 9) return false;
    int idx = digit1To9 - 1;
    if (idx >= static_cast<int>(cfg.presets.size())) return false;
    return selectPreset(idx, cfg, nowMs, true, lang);
}

bool RadialController::enqueueSlotForTest(
    RadialSlot const&   slot,
    RadialConfig const& cfg,
    int64_t             nowMs,
    ResolvedLang        lang
) {
    if (slot.type == ActionType::Submenu) return false;
    return executeSlot(slot, "__test__", cfg, nowMs, lang);
}

void RadialController::update(RadialConfig const& cfg, int64_t nowMs) {
    if (mArming && !mOpen) {
        if (nowMs - mPressStartedMs >= cfg.holdDelayMs) {
            mArming            = false;
            mOpen              = true;
            mOpenedAtMs        = nowMs;
            mPointerX          = 0.0f;
            mPointerY          = 0.0f;
            mHoveredIndex      = -1;
            mHoverSinceMs      = nowMs;
            mSubmenuGraceEndMs = nowMs;
        }
        return;
    }

    if (!mOpen) return;

    if (cfg.submenuHoverMs > 0 && mHoveredIndex >= 0) {
        auto const* slots = resolveCurrentSlots(cfg);
        if (slots && mHoveredIndex < static_cast<int>(slots->size())) {
            auto const& slot = (*slots)[static_cast<size_t>(mHoveredIndex)];
            if (slot.type == ActionType::Submenu && !slot.children.empty()
                && static_cast<int>(mSubmenuPath.size()) < kMaxSubmenuDepth
                && (nowMs - mHoverSinceMs) >= cfg.submenuHoverMs) {
                enterSubmenu(mHoveredIndex, cfg, nowMs);
            }
        }
    }
}

std::vector<QueuedAction> RadialController::pollDueActions(int64_t nowMs) {
    std::vector<QueuedAction> due;
    if (mPendingActions.empty()) return due;

    auto it = mPendingActions.begin();
    while (it != mPendingActions.end()) {
        if (it->executeAtMs <= nowMs) {
            due.push_back(std::move(*it));
            it = mPendingActions.erase(it);
        } else {
            ++it;
        }
    }
    return due;
}

void RadialController::clearPendingActions() {
    mPendingActions.clear();
}

void RadialController::showToast(std::string text, int64_t nowMs, int durationMs) {
    mToastText    = std::move(text);
    mToastUntilMs = nowMs + std::max(300, durationMs);
}

RadialViewSnapshot RadialController::buildSnapshot(RadialConfig const& cfg, int64_t nowMs) const {
    RadialViewSnapshot snap;
    snap.open              = mOpen;
    snap.pointerX          = mPointerX;
    snap.pointerY          = mPointerY;
    snap.hoveredIndex      = mHoveredIndex;
    snap.activePresetIndex = mActivePresetIndex;
    snap.presetCount       = static_cast<int>(cfg.presets.size());
    snap.inSubmenu         = !mSubmenuPath.empty();
    snap.manualOverride    = mManualOverride;
    snap.serverDisplay     = mServer.displayLabel();

    if (mOpen) {
        float elapsed     = static_cast<float>(std::max<int64_t>(0, nowMs - mOpenedAtMs));
        float t           = std::clamp(elapsed / 100.0f, 0.0f, 1.0f);
        float inv         = 1.0f - t;
        snap.openProgress = 1.0f - (inv * inv * inv);
    }

    if (!cfg.presets.empty() && mActivePresetIndex >= 0
        && mActivePresetIndex < static_cast<int>(cfg.presets.size())) {
        snap.presetName = cfg.presets[static_cast<size_t>(mActivePresetIndex)].name;
        snap.breadcrumb = buildBreadcrumb(cfg);
        if (auto const* slots = resolveCurrentSlots(cfg)) {
            snap.visibleSlots = *slots;
            if (mOpen && cfg.submenuHoverMs > 0 && mHoveredIndex >= 0
                && mHoveredIndex < static_cast<int>(slots->size())
                && (*slots)[static_cast<size_t>(mHoveredIndex)].type == ActionType::Submenu
                && !(*slots)[static_cast<size_t>(mHoveredIndex)].children.empty()) {
                float ratio = static_cast<float>(std::max<int64_t>(0, nowMs - mHoverSinceMs))
                            / static_cast<float>(cfg.submenuHoverMs);
                snap.submenuHoverRatio = std::clamp(ratio, 0.0f, 1.0f);
            }
        }
    }

    if (!mToastText.empty() && nowMs < mToastUntilMs) {
        int64_t rem = mToastUntilMs - nowMs;
        float   a   = std::clamp(static_cast<float>(rem) / 350.0f, 0.0f, 1.0f);
        snap.toast  = ToastState{mToastText, a};
    }

    return snap;
}

std::vector<RadialSlot> const* RadialController::resolveCurrentSlots(RadialConfig const& cfg) const noexcept {
    if (cfg.presets.empty() || mActivePresetIndex < 0
        || mActivePresetIndex >= static_cast<int>(cfg.presets.size())) {
        return nullptr;
    }
    std::vector<RadialSlot> const* cur = &cfg.presets[static_cast<size_t>(mActivePresetIndex)].slots;
    for (int idx : mSubmenuPath) {
        if (idx < 0 || idx >= static_cast<int>(cur->size())) return cur;
        auto const& s = (*cur)[static_cast<size_t>(idx)];
        if (s.type != ActionType::Submenu || s.children.empty()) return cur;
        cur = &s.children;
    }
    return cur;
}

std::string RadialController::buildBreadcrumb(RadialConfig const& cfg) const {
    if (cfg.presets.empty() || mActivePresetIndex < 0
        || mActivePresetIndex >= static_cast<int>(cfg.presets.size())) {
        return "";
    }
    std::string                    out = cfg.presets[static_cast<size_t>(mActivePresetIndex)].name;
    std::vector<RadialSlot> const* cur = &cfg.presets[static_cast<size_t>(mActivePresetIndex)].slots;
    for (int idx : mSubmenuPath) {
        if (idx < 0 || idx >= static_cast<int>(cur->size())) break;
        auto const& s = (*cur)[static_cast<size_t>(idx)];
        out += " > " + s.label;
        if (s.type != ActionType::Submenu || s.children.empty()) break;
        cur = &s.children;
    }
    return out;
}

std::string RadialController::buildSlotCooldownKey(int slotIndex) const {
    std::string k = std::to_string(mActivePresetIndex);
    for (int idx : mSubmenuPath) {
        k += "/";
        k += std::to_string(idx);
    }
    k += ":";
    k += std::to_string(slotIndex);
    return k;
}

void RadialController::recomputeHover(RadialConfig const& cfg, int64_t nowMs) {
    auto const* slots = resolveCurrentSlots(cfg);
    if (!slots || slots->empty()) {
        mHoveredIndex = -1;
        return;
    }

    int const count = static_cast<int>(slots->size());
    float     dist  = std::sqrt(mPointerX * mPointerX + mPointerY * mPointerY);

    // Schmitt trigger deadzone: slightly smaller radius to return to deadzone once a sector is selected.
    float dzThreshold = (mHoveredIndex >= 0) ? (cfg.deadzoneRadius * kDeadzoneExitFactor) : cfg.deadzoneRadius;
    if (dist < dzThreshold) {
        if (mHoveredIndex != -1) {
            mHoveredIndex = -1;
            mHoverSinceMs = nowMs;
        }
        return;
    }

    int candidate = sectorIndexFromVector(mPointerX, mPointerY, count);

    // Angular hysteresis: keep current sector while pointer stays within halfSpan + hysteresis of its center.
    if (mHoveredIndex >= 0 && mHoveredIndex < count && candidate != mHoveredIndex) {
        float halfSpan = (kTwoPi / static_cast<float>(count)) * 0.5f;
        float ptrAngle = std::atan2(mPointerY, mPointerX);
        float curAngle = sectorCenterAngle(mHoveredIndex, count);
        float delta    = std::abs(wrapAnglePi(ptrAngle - curAngle));
        if (delta < halfSpan + kAngleHysteresisRad) {
            return;
        }
    }

    if (candidate != mHoveredIndex) {
        mHoveredIndex = candidate;
        mHoverSinceMs = nowMs;
    }
}

bool RadialController::executeSlot(
    RadialSlot const&   slot,
    std::string const&  slotKey,
    RadialConfig const& cfg,
    int64_t             nowMs,
    ResolvedLang        lang
) {
    // 1) Independent per-slot cooldown check (if configured).
    if (slot.cooldownMs >= 0) {
        auto it = mSlotLastExecMs.find(slotKey);
        if (it != mSlotLastExecMs.end() && slot.cooldownMs > 0 && (nowMs - it->second) < slot.cooldownMs) {
            if (cfg.showPresetToast) {
                int remMs = static_cast<int>(slot.cooldownMs - (nowMs - it->second));
                showToast(std::string(tr(lang, "hud.toast.cooldown")) + " (" + std::to_string(remMs) + "ms)", nowMs, 900);
            }
            return false;
        }
    } else if (cfg.globalCooldownMs > 0 && (nowMs - mLastGlobalExecMs) < cfg.globalCooldownMs) {
        // 2) Global cooldown for slots using default global cooldown.
        if (cfg.showPresetToast) {
            int remMs = static_cast<int>(cfg.globalCooldownMs - (nowMs - mLastGlobalExecMs));
            showToast(std::string(tr(lang, "hud.toast.cooldown")) + " (" + std::to_string(remMs) + "ms)", nowMs, 900);
        }
        return false;
    }

    if (slot.type == ActionType::Macro) {
        if (slot.steps.empty()) return false;
        if (mPendingActions.size() >= kMaxPendingActions) return false;
        int64_t cursorMs = nowMs;
        int     queued   = 0;
        for (auto const& st : slot.steps) {
            if (isBlankString(st.value)) continue;
            if (mPendingActions.size() >= kMaxPendingActions) break;
            cursorMs += std::max(0, st.delayMs);
            mPendingActions.push_back(QueuedAction{st.type, st.value, slot.label, cursorMs});
            ++queued;
        }
        if (queued == 0) return false;
        mLastGlobalExecMs        = nowMs;
        mSlotLastExecMs[slotKey] = nowMs;
        if (cfg.showPresetToast) {
            showToast(slot.label + " (" + std::to_string(queued) + " " + tr(lang, "hud.steps") + ")", nowMs, 1400);
        }
        return true;
    }

    if (isBlankString(slot.value)) return false;
    if (mPendingActions.size() >= kMaxPendingActions) return false;
    mPendingActions.push_back(QueuedAction{slot.type, slot.value, slot.label, nowMs});
    mLastGlobalExecMs        = nowMs;
    mSlotLastExecMs[slotKey] = nowMs;
    if (cfg.showPresetToast) {
        showToast(slot.label + " -> " + slot.value, nowMs, 1300);
    }
    return true;
}

bool RadialController::enterSubmenu(int slotIdx, RadialConfig const& cfg, int64_t nowMs) {
    auto const* slots = resolveCurrentSlots(cfg);
    if (!slots || slotIdx < 0 || slotIdx >= static_cast<int>(slots->size())) return false;
    auto const& slot = (*slots)[static_cast<size_t>(slotIdx)];
    if (slot.type != ActionType::Submenu || slot.children.empty()) return false;
    if (static_cast<int>(mSubmenuPath.size()) >= kMaxSubmenuDepth) return false;

    mSubmenuPath.push_back(slotIdx);
    mPointerX          = 0.0f;
    mPointerY          = 0.0f;
    mHoveredIndex      = -1;
    mHoverSinceMs      = nowMs;
    mOpenedAtMs        = nowMs;
    mSubmenuGraceEndMs = nowMs + kSubmenuGraceMs;
    return true;
}

void RadialController::clampPresetAndPath(RadialConfig const& cfg) {
    if (cfg.presets.empty()) {
        mActivePresetIndex = 0;
        mSubmenuPath.clear();
        return;
    }
    if (mActivePresetIndex < 0 || mActivePresetIndex >= static_cast<int>(cfg.presets.size())) {
        mActivePresetIndex = std::max(0, findPresetIndex(cfg, cfg.defaultPresetId));
        mSubmenuPath.clear();
    }
}

} // namespace radial
