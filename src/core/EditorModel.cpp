#include "core/EditorModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace radial::editor {
namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

char vkToFallbackAscii(int vk, bool shift) noexcept {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<char>(shift ? vk : (vk - 'A' + 'a'));
    }
    if (vk >= '0' && vk <= '9') {
        static constexpr char kShiftedDigits[10] = {')', '!', '@', '#', '$', '%', '^', '&', '*', '('};
        return shift ? kShiftedDigits[vk - '0'] : static_cast<char>(vk);
    }
    if (vk == kVkSpace) return ' ';
    switch (vk) {
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    case 0xE2: return shift ? '_' : '\\'; // JIS VK_OEM_102 fallback
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    default: return 0;
    }
}

std::string makeUniquePresetId(RadialConfig const& cfg, std::string base) {
    std::string slug;
    for (unsigned char c : base) {
        if (std::isalnum(c)) slug.push_back(static_cast<char>(std::tolower(c)));
        else if (c == '_' || c == '-' || c == ' ') slug.push_back('_');
    }
    if (slug.empty()) slug = "preset";
    std::string cand = slug;
    int         n    = 2;
    while (findPresetIndex(cfg, cand) >= 0) {
        cand = slug + "_" + std::to_string(n++);
    }
    return cand;
}

float normRatio(float v, float lo, float hi) noexcept {
    if (!(hi > lo)) return 0.0f;
    return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
}

std::string fmtFloat(float v, int decimals = 2) {
    char buf[32]{};
    if (decimals == 0) std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(v)));
    else if (decimals == 1) std::snprintf(buf, sizeof(buf), "%.1f", v);
    else std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

} // namespace

ResolvedLang EditorModel::resolvedLang() const noexcept {
    return resolveUiLanguage(mDraft.language, mSystemIsJapanese);
}

void EditorModel::beginSession(
    RadialConfig const&   cfg,
    ServerIdentity const& server,
    int                   initialPresetIdx,
    bool                  systemIsJapanese
) {
    mSystemIsJapanese  = systemIsJapanese;
    mActiveTab         = EditorTab::Wheel;
    mDraft             = cfg;
    mUndoStack.clear();
    mServer            = server;
    mSelectedPreset    = initialPresetIdx;
    mSubmenuPath.clear();
    mSelectedSlot      = 0;
    mSelectedMacroStep = 0;
    mMacroPage         = 0;
    mSelectedRule      = 0;
    mSettingsSlidePage = 0;
    mSettingsTogPage   = 0;
    mFocusedField      = TextFieldId::None;
    mCursorByte        = 0;
    mSelectedAll       = false;
    mCaptureTarget     = CaptureTarget::None;
    mDraggingSliderId.clear();
    mDirty             = false;
    mConfirmClose      = false;
    mConfirmReset      = false;
    ensureValidSelection();
    mStatus            = tr(resolvedLang(), "ed.status.welcome");
}

void EditorModel::pushUndoSnapshot() {
    mUndoStack.push_back(mDraft);
    if (mUndoStack.size() > kMaxUndoDepth) {
        mUndoStack.erase(mUndoStack.begin());
    }
}

bool EditorModel::undo() {
    if (mUndoStack.empty()) return false;
    mDraft = std::move(mUndoStack.back());
    mUndoStack.pop_back();
    mDirty        = true;
    mConfirmClose = false;
    mConfirmReset = false;
    mFocusedField = TextFieldId::None;
    ensureValidSelection();
    mStatus = tr(resolvedLang(), "ed.status.undone");
    return true;
}

void EditorModel::ensureValidSelection() noexcept {
    if (mDraft.presets.empty()) {
        mDraft = makeDefaultConfig();
    }
    mSelectedPreset = std::clamp(mSelectedPreset, 0, static_cast<int>(mDraft.presets.size()) - 1);

    auto& preset = mDraft.presets[static_cast<size_t>(mSelectedPreset)];
    if (preset.serverRules.empty()) {
        mSelectedRule = 0;
    } else {
        mSelectedRule = std::clamp(mSelectedRule, 0, static_cast<int>(preset.serverRules.size()) - 1);
    }

    std::vector<RadialSlot>* cur = &preset.slots;
    for (size_t i = 0; i < mSubmenuPath.size(); ++i) {
        int idx = mSubmenuPath[i];
        if (idx < 0 || idx >= static_cast<int>(cur->size())
            || (*cur)[static_cast<size_t>(idx)].type != ActionType::Submenu) {
            mSubmenuPath.resize(i);
            break;
        }
        cur = &(*cur)[static_cast<size_t>(idx)].children;
    }

    if (cur->empty()) {
        mSelectedSlot = -1;
    } else {
        mSelectedSlot = std::clamp(mSelectedSlot, 0, static_cast<int>(cur->size()) - 1);
    }

    if (auto* slot = currentSlot(); slot && slot->type == ActionType::Macro && !slot->steps.empty()) {
        mSelectedMacroStep = std::clamp(mSelectedMacroStep, 0, static_cast<int>(slot->steps.size()) - 1);
        int maxPage        = std::max(0, (static_cast<int>(slot->steps.size()) - 1) / 2);
        mMacroPage         = std::clamp(mMacroPage, 0, maxPage);
    } else {
        mSelectedMacroStep = 0;
        mMacroPage         = 0;
    }

    if (auto const* r = currentRule()) {
        mPortEditBuf = std::to_string(r->port);
    } else {
        mPortEditBuf = "0";
    }
}

void EditorModel::ensureSelectedRule(RadialPreset& p) {
    if (p.serverRules.empty()) {
        p.serverRules.push_back(ServerRule{});
        mSelectedRule = 0;
    } else {
        mSelectedRule = std::clamp(mSelectedRule, 0, static_cast<int>(p.serverRules.size()) - 1);
    }
}

RadialPreset* EditorModel::currentPreset() noexcept {
    if (mDraft.presets.empty() || mSelectedPreset < 0
        || mSelectedPreset >= static_cast<int>(mDraft.presets.size())) {
        return nullptr;
    }
    return &mDraft.presets[static_cast<size_t>(mSelectedPreset)];
}

RadialPreset const* EditorModel::currentPreset() const noexcept {
    if (mDraft.presets.empty() || mSelectedPreset < 0
        || mSelectedPreset >= static_cast<int>(mDraft.presets.size())) {
        return nullptr;
    }
    return &mDraft.presets[static_cast<size_t>(mSelectedPreset)];
}

ServerRule* EditorModel::currentRule() noexcept {
    auto* p = currentPreset();
    if (!p || p->serverRules.empty()) return nullptr;
    int idx = std::clamp(mSelectedRule, 0, static_cast<int>(p->serverRules.size()) - 1);
    return &p->serverRules[static_cast<size_t>(idx)];
}

ServerRule const* EditorModel::currentRule() const noexcept {
    auto const* p = currentPreset();
    if (!p || p->serverRules.empty()) return nullptr;
    int idx = std::clamp(mSelectedRule, 0, static_cast<int>(p->serverRules.size()) - 1);
    return &p->serverRules[static_cast<size_t>(idx)];
}

std::vector<RadialSlot>* EditorModel::currentSlotList() noexcept {
    auto* p = currentPreset();
    if (!p) return nullptr;
    std::vector<RadialSlot>* cur = &p->slots;
    for (int idx : mSubmenuPath) {
        if (idx < 0 || idx >= static_cast<int>(cur->size())) break;
        auto& s = (*cur)[static_cast<size_t>(idx)];
        if (s.type != ActionType::Submenu) break;
        cur = &s.children;
    }
    return cur;
}

std::vector<RadialSlot> const* EditorModel::currentSlotList() const noexcept {
    auto const* p = currentPreset();
    if (!p) return nullptr;
    std::vector<RadialSlot> const* cur = &p->slots;
    for (int idx : mSubmenuPath) {
        if (idx < 0 || idx >= static_cast<int>(cur->size())) break;
        auto const& s = (*cur)[static_cast<size_t>(idx)];
        if (s.type != ActionType::Submenu) break;
        cur = &s.children;
    }
    return cur;
}

RadialSlot* EditorModel::currentSlot() noexcept {
    auto* list = currentSlotList();
    if (!list || mSelectedSlot < 0 || mSelectedSlot >= static_cast<int>(list->size())) return nullptr;
    return &(*list)[static_cast<size_t>(mSelectedSlot)];
}

RadialSlot const* EditorModel::currentSlot() const noexcept {
    auto const* list = currentSlotList();
    if (!list || mSelectedSlot < 0 || mSelectedSlot >= static_cast<int>(list->size())) return nullptr;
    return &(*list)[static_cast<size_t>(mSelectedSlot)];
}

std::string EditorModel::currentBreadcrumb() const {
    auto const* p = currentPreset();
    if (!p) return "";
    std::string                    out = p->name;
    std::vector<RadialSlot> const* cur = &p->slots;
    for (int idx : mSubmenuPath) {
        if (idx < 0 || idx >= static_cast<int>(cur->size())) break;
        out += " > " + (*cur)[static_cast<size_t>(idx)].label;
        cur = &(*cur)[static_cast<size_t>(idx)].children;
    }
    return out;
}

bool EditorModel::createPresetForCurrentServer() {
    if (static_cast<int>(mDraft.presets.size()) >= kMaxPresets) {
        mStatus = (resolvedLang() == ResolvedLang::Ja) ? "プリセット上限に達しました" : "Maximum presets reached";
        return false;
    }
    pushUndoSnapshot();

    RadialPreset np;
    ServerRule   rule;
    bool         hasSpecificRule = false;

    if (mServer.inWorld && !mServer.isLocal) {
        std::string hostPat = suggestHostPattern(mServer);
        std::string title   = !mServer.serverName.empty() ? mServer.serverName
                            : (!mServer.host.empty() ? mServer.host : (!mServer.worldName.empty() ? mServer.worldName : mServer.ip));
        if (title.empty()) title = "Server";
        np.name = truncateUtf8Bytes(title, 40);
        np.id   = makeUniquePresetId(mDraft, title);

        // Avoid overly brittle AND rules and avoid empty catch-all External rules (e.g. LAN/WebRTC).
        if (!hostPat.empty()) {
            rule.hostPattern = hostPat;
            rule.scope       = ConnectionScope::External;
            rule.priority    = 120;
            if (mServer.port > 0 && mServer.port != 19132) rule.port = mServer.port;
            hasSpecificRule = true;
        } else if (!mServer.serverName.empty()) {
            rule.serverNamePattern = "*" + mServer.serverName + "*";
            rule.scope             = ConnectionScope::External;
            rule.priority          = 110;
            hasSpecificRule        = true;
        } else if (!mServer.worldName.empty()) {
            rule.worldNamePattern = "*" + mServer.worldName + "*";
            rule.scope            = ConnectionScope::External;
            rule.priority         = 110;
            hasSpecificRule       = true;
        }
    } else if (mServer.inWorld && mServer.isLocal) {
        std::string title = mServer.worldName.empty() ? "Local Custom" : mServer.worldName;
        np.name           = truncateUtf8Bytes(title, 40);
        np.id             = makeUniquePresetId(mDraft, title);
        if (!mServer.worldName.empty()) {
            rule.worldNamePattern = "*" + mServer.worldName + "*";
            rule.scope            = ConnectionScope::Local;
            rule.priority         = 120;
            hasSpecificRule       = true;
        }
    } else {
        np.name = "Custom Preset";
        np.id   = makeUniquePresetId(mDraft, "custom");
    }

    if (hasSpecificRule) {
        np.serverRules.push_back(std::move(rule));
    }

    np.slots = {
        RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"},
        RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#34D399"},
        RadialSlot{"Spawn", "/spawn", ActionType::Command, "/spawn", "#60A5FA"},
        RadialSlot{"Thanks", "Chat", ActionType::Chat, "ありがとう！", "#F59E0B"},
    };

    mDraft.presets.push_back(std::move(np));
    mSelectedPreset = static_cast<int>(mDraft.presets.size()) - 1;
    mSelectedRule   = 0;
    mSubmenuPath.clear();
    mSelectedSlot = 0;
    mDirty        = true;
    mStatus       = (resolvedLang() == ResolvedLang::Ja)
                      ? (hasSpecificRule ? "現在のサーバー用プリセットを作成しました" : "プリセットを作成しました（手動でルールを設定してください）")
                      : (hasSpecificRule ? "Created preset for current server/world" : "Created preset (set a server rule manually)");
    ensureValidSelection();
    return true;
}

int EditorModel::hitTestWheelPreview(WheelPreviewLayout const& wp, float mx, float my) noexcept {
    if (!wp.visible || wp.slots.empty()) return -1;
    float dx   = mx - wp.cx;
    float dy   = my - wp.cy;
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < wp.innerR - 2.0f) return -2;
    if (dist > wp.outerR + 8.0f) return -1;

    int   count = static_cast<int>(wp.slots.size());
    float angle = std::atan2(dy, dx);
    float span  = kTwoPi / static_cast<float>(count);
    float shift = angle + (kPi * 0.5f) + (span * 0.5f);
    shift       = std::fmod(shift, kTwoPi);
    if (shift < 0.0f) shift += kTwoPi;
    int idx = static_cast<int>(std::floor(shift / span));
    return std::clamp(idx, 0, count - 1);
}

EditorLayout EditorModel::buildLayout(float screenW, float screenH) const {
    EditorLayout L;
    ResolvedLang lang = resolvedLang();
    L.lang            = lang;
    L.activeTab       = mActiveTab;
    L.confirmingClose = mConfirmClose;

    // Responsive bounds that NEVER clip off-screen even at high GUI scales.
    float maxPw = std::max(260.0f, screenW - 12.0f);
    float maxPh = std::max(180.0f, screenH - 12.0f);
    float pw    = std::min(maxPw, 540.0f);
    float ph    = std::min(maxPh, 304.0f);
    float px    = std::floor((screenW - pw) * 0.5f);
    float py    = std::floor((screenH - ph) * 0.5f);

    L.panel  = {px, py, px + pw, py + ph};
    L.header = {px, py, px + pw, py + 22.0f};
    L.tabBar = {px, py + 22.0f, px + pw, py + 38.0f};

    // Header buttons (right aligned) — compute headerTextArea strictly to the left of them!
    float bx = px + pw - 4.0f;
    auto addHeaderBtn = [&](std::string id, std::string label, float w, std::string hint, bool accent = false, bool danger = false, bool success = false, bool disabled = false) {
        bx -= w;
        EditorButton btn{{bx, py + 4.0f, bx + w, py + 18.0f}, std::move(id), std::move(label), false, accent, danger, success, disabled};
        btn.hint = std::move(hint);
        L.buttons.push_back(std::move(btn));
        bx -= 3.0f;
    };
    addHeaderBtn("close", tr(lang, "ed.close"), 34.0f, tr(lang, "hint.close"), false, true);
    addHeaderBtn("save", tr(lang, mDirty ? "ed.save_dirty" : "ed.save"), 38.0f, tr(lang, "hint.save"), false, false, mDirty);
    addHeaderBtn("undo", tr(lang, "ed.undo"), 30.0f, tr(lang, "hint.undo"), false, false, false, mUndoStack.empty());
    addHeaderBtn("reload", tr(lang, "ed.reload"), 36.0f, tr(lang, "hint.reload"));
    addHeaderBtn("lang_toggle", lang == ResolvedLang::Ja ? "JA" : "EN", 22.0f, tr(lang, "hint.lang"), true);

    L.headerTextArea = {px + 6.0f, py + 2.0f, std::max(px + 60.0f, bx - 4.0f), py + 21.0f};

    // Tab bar (3 balanced responsive tabs)
    float tabAvailW = pw - 16.0f;
    float tabW      = std::min(108.0f, (tabAvailW - 8.0f) / 3.0f);
    float tx        = px + 5.0f;
    L.buttons.push_back(EditorButton{{tx, py + 24.0f, tx + tabW, py + 36.0f}, "tab:wheel", tr(lang, "ed.tab.wheel"), mActiveTab == EditorTab::Wheel});
    tx += tabW + 4.0f;
    L.buttons.push_back(EditorButton{{tx, py + 24.0f, tx + tabW, py + 36.0f}, "tab:servers", tr(lang, "ed.tab.servers"), mActiveTab == EditorTab::Servers});
    tx += tabW + 4.0f;
    L.buttons.push_back(EditorButton{{tx, py + 24.0f, tx + tabW, py + 36.0f}, "tab:settings", tr(lang, "ed.tab.settings"), mActiveTab == EditorTab::Settings});

    float bodyTop = py + 41.0f;

    // Optional unsaved changes confirmation bar (sits cleanly above body, never overlaps content).
    if (mConfirmClose) {
        L.confirmBanner = {px + 5.0f, bodyTop, px + pw - 5.0f, bodyTop + 17.0f};
        float cbx       = L.confirmBanner.x1 - 3.0f;
        auto addConfBtn = [&](std::string id, std::string lbl, float w, bool danger = false, bool success = false) {
            cbx -= w;
            L.buttons.push_back(EditorButton{{cbx, bodyTop + 2.0f, cbx + w, bodyTop + 15.0f}, std::move(id), std::move(lbl), false, false, danger, success});
            cbx -= 3.0f;
        };
        addConfBtn("confirm_stay", tr(lang, "ed.unsaved.cancel"), 46.0f);
        addConfBtn("confirm_discard", tr(lang, "ed.unsaved.discard"), 56.0f, true);
        addConfBtn("confirm_save_close", tr(lang, "ed.unsaved.save_close"), 64.0f, false, true);
        bodyTop += 20.0f;
    }

    float bodyBot = py + ph - 15.0f;
    float pad     = 5.0f;
    float gap     = 5.0f;
    float usableW = pw - pad * 2.0f - gap;
    float leftW   = std::floor(usableW * (mActiveTab == EditorTab::Wheel ? 0.48f : 0.45f));
    float rightW  = usableW - leftW;

    L.leftPane  = {px + pad, bodyTop, px + pad + leftW, bodyBot};
    L.rightPane = {L.leftPane.x1 + gap, bodyTop, L.leftPane.x1 + gap + rightW, bodyBot};

    auto pushTextBox = [&](geom::RectF r, TextFieldId fid, std::string cap, std::string val, std::string hint = {}) {
        bool        foc  = (mFocusedField == fid);
        float       clrW = val.empty() ? 0.0f : 12.0f;
        geom::RectF clrR = val.empty() ? geom::RectF{} : geom::RectF{r.x1 - clrW, r.y0 + 1.0f, r.x1 - 1.0f, r.y1 - 1.0f};
        L.textBoxes.push_back(EditorTextBox{
            r,
            clrR,
            fid,
            std::move(cap),
            std::move(val),
            {},
            foc,
            foc && mSelectedAll,
            foc ? std::min(mCursorByte, getFieldValue(fid).size()) : 0,
            std::move(hint),
        });
    };

    auto pushHintBtn = [&](EditorButton b, std::string hint) {
        b.hint = std::move(hint);
        L.buttons.push_back(std::move(b));
    };

    // =========================================================================
    // TAB 1: WHEEL VISUAL EDITOR
    // =========================================================================
    if (mActiveTab == EditorTab::Wheel) {
        float lx = L.leftPane.x0 + 4.0f;
        float lw = L.leftPane.width() - 8.0f;
        float ly = L.leftPane.y0 + 4.0f;

        int         totalP = static_cast<int>(mDraft.presets.size());
        std::string pLabel = currentPreset()
                               ? (currentPreset()->name + " (" + std::to_string(mSelectedPreset + 1) + "/" + std::to_string(totalP) + ")")
                               : "Preset";
        L.buttons.push_back(EditorButton{{lx, ly, lx + 16.0f, ly + 13.0f}, "preset_prev", "<"});
        L.buttons.push_back(EditorButton{{lx + 18.0f, ly, lx + lw - 18.0f, ly + 13.0f}, "tab:servers", pLabel, false, true});
        L.buttons.push_back(EditorButton{{lx + lw - 16.0f, ly, lx + lw, ly + 13.0f}, "preset_next", ">"});
        ly += 15.0f;

        if (!mSubmenuPath.empty()) {
            L.buttons.push_back(EditorButton{{lx, ly, lx + lw, ly + 12.0f}, "submenu_up", std::string(tr(lang, "ed.submenu.back")) + " (" + currentBreadcrumb() + ")", false, true});
        } else {
            float cw = (lw - 6.0f) / 3.0f;
            L.buttons.push_back(EditorButton{{lx, ly, lx + cw, ly + 12.0f}, "preset_add", tr(lang, "ed.preset.new"), false, true});
            L.buttons.push_back(EditorButton{{lx + cw + 3.0f, ly, lx + cw * 2.0f + 3.0f, ly + 12.0f}, "preset_dup", tr(lang, "ed.preset.dup")});
            L.buttons.push_back(EditorButton{{lx + (cw + 3.0f) * 2.0f, ly, lx + lw, ly + 12.0f}, "preset_del", tr(lang, "ed.preset.del"), false, false, true, false, totalP <= 1});
        }
        ly += 15.0f;

        // Interactive circular wheel preview (reflects live radius ratio, deadzone, opacity, fontScale!)
        float botBarY = L.leftPane.y1 - 16.0f;
        float availH  = std::max(60.0f, botBarY - ly - 3.0f);
        float maxR    = std::min(lw * 0.44f, availH * 0.46f);
        float outerR  = std::clamp(maxR, 28.0f, 76.0f);
        float ratioIn = mDraft.innerRadius / std::max(50.0f, mDraft.outerRadius);
        float ratioDz = mDraft.deadzoneRadius / std::max(50.0f, mDraft.outerRadius);
        float innerR  = std::clamp(outerR * ratioIn, 12.0f, outerR - 14.0f);
        float deadR   = std::clamp(outerR * ratioDz, 7.0f, outerR - 8.0f);

        L.wheelPreview.visible   = true;
        L.wheelPreview.bounds    = {lx, ly, lx + lw, botBarY - 2.0f};
        L.wheelPreview.cx        = lx + lw * 0.5f;
        L.wheelPreview.cy        = ly + availH * 0.5f;
        L.wheelPreview.outerR    = outerR;
        L.wheelPreview.innerR    = innerR;
        L.wheelPreview.deadzoneR = deadR;
        L.wheelPreview.bgOpacity = mDraft.backgroundOpacity;
        L.wheelPreview.fontScale = mDraft.fontScale;
        L.wheelPreview.selected  = mSelectedSlot;
        L.wheelPreview.inSubmenu = !mSubmenuPath.empty();
        if (auto const* sl = currentSlotList()) {
            L.wheelPreview.slots = *sl;
        }

        // Bottom 5 wheel buttons: [+Add] [Dup] [<Move] [Move>] [Del]
        int   slotCount = static_cast<int>(L.wheelPreview.slots.size());
        float bw5       = (lw - 8.0f) / 5.0f;
        L.buttons.push_back(EditorButton{{lx, botBarY, lx + bw5, botBarY + 13.0f}, "slot_add", tr(lang, "ed.slot.add"), false, true, false, false, slotCount >= kMaxSlotsPerMenu});
        L.buttons.push_back(EditorButton{{lx + (bw5 + 2.0f), botBarY, lx + bw5 * 2.0f + 2.0f, botBarY + 13.0f}, "slot_dup", tr(lang, "ed.slot.dup"), false, false, false, false, slotCount >= kMaxSlotsPerMenu || slotCount == 0});
        pushHintBtn(EditorButton{{lx + (bw5 + 2.0f) * 2.0f, botBarY, lx + bw5 * 3.0f + 4.0f, botBarY + 13.0f}, "slot_up", tr(lang, "ed.slot.prev")}, tr(lang, "hint.slot_move"));
        pushHintBtn(EditorButton{{lx + (bw5 + 2.0f) * 3.0f, botBarY, lx + bw5 * 4.0f + 6.0f, botBarY + 13.0f}, "slot_down", tr(lang, "ed.slot.next")}, tr(lang, "hint.slot_move"));
        L.buttons.push_back(EditorButton{{lx + (bw5 + 2.0f) * 4.0f, botBarY, lx + lw, botBarY + 13.0f}, "slot_del", tr(lang, "ed.slot.del"), false, false, true, false, slotCount <= kMinSlotsPerMenu});

        // --- Right Pane: Selected Slot Inspector ---
        float rx = L.rightPane.x0 + 5.0f;
        float rw = L.rightPane.width() - 10.0f;
        float ry = L.rightPane.y0 + 15.0f;

        if (auto const* s = currentSlot()) {
            // Test Run button in top-right of inspector pane
            if (s->type != ActionType::Submenu) {
                pushHintBtn(EditorButton{
                    {L.rightPane.x1 - 44.0f, L.rightPane.y0 + 2.0f, L.rightPane.x1 - 5.0f, L.rightPane.y0 + 12.5f},
                    "slot_test",
                    tr(lang, "ed.slot.test"),
                    false, false, false, true,
                }, tr(lang, "hint.slot_test"));
            }

            pushTextBox({rx, ry + 7.5f, rx + rw, ry + 20.5f}, TextFieldId::SlotLabel, tr(lang, "ed.slot.label"), s->label, tr(lang, "hint.slot_label"));
            ry += 24.5f;

            // 5 localized action type buttons with hover hints
            struct TypeSpec { ActionType type; char const* lblKey; char const* hintKey; };
            static constexpr TypeSpec kTypes[] = {
                {ActionType::Command,       "ed.type.cmd",   "hint.type_cmd"},
                {ActionType::Chat,          "ed.type.chat",  "hint.type_chat"},
                {ActionType::ClientCommand, "ed.type.cli",   "hint.type_cli"},
                {ActionType::Macro,         "ed.type.macro", "hint.type_macro"},
                {ActionType::Submenu,       "ed.type.sub",   "hint.type_sub"},
            };
            float tw = (rw - 8.0f) / 5.0f;
            for (int i = 0; i < 5; ++i) {
                float tx2 = rx + static_cast<float>(i) * (tw + 2.0f);
                pushHintBtn(EditorButton{
                    {tx2, ry, tx2 + tw, ry + 13.5f},
                    std::string("type:") + std::string(actionTypeToString(kTypes[i].type)),
                    tr(lang, kTypes[i].lblKey),
                    s->type == kTypes[i].type,
                }, tr(lang, kTypes[i].hintKey));
            }
            ry += 17.5f;

            // Reserve bottom 60px strictly for 2-row 16-color swatches + Cooldown with clear section headers
            float const footerGuardY = L.rightPane.y1 - 61.0f;

            if (s->type == ActionType::Command || s->type == ActionType::Chat || s->type == ActionType::ClientCommand) {
                char const* capKey  = (s->type == ActionType::Chat)          ? "ed.slot.chat_cap"
                                    : (s->type == ActionType::ClientCommand) ? "ed.slot.cli_cap"
                                                                             : "ed.slot.cmd_cap";
                char const* hintKey = (s->type == ActionType::Chat)          ? "hint.slot_chat"
                                    : (s->type == ActionType::ClientCommand) ? "hint.slot_cli"
                                                                             : "hint.slot_cmd";
                pushTextBox({rx, ry + 7.5f, rx + rw, ry + 20.5f}, TextFieldId::SlotValue, tr(lang, capKey), s->value, tr(lang, hintKey));
                ry += 25.0f;

                if (ry + 21.0f <= footerGuardY) {
                    pushTextBox({rx, ry + 7.5f, rx + rw, ry + 20.5f}, TextFieldId::SlotSubtitle, tr(lang, "ed.slot.subtitle"), s->subtitle, tr(lang, "hint.slot_subtitle"));
                }
            } else if (s->type == ActionType::Submenu) {
                bool atMaxDepth = static_cast<int>(mSubmenuPath.size()) >= kMaxSubmenuDepth;
                L.buttons.push_back(EditorButton{
                    {rx, ry, rx + rw, ry + 16.0f},
                    "submenu_open",
                    atMaxDepth ? std::string(tr(lang, "ed.slot.sub_max"))
                               : (std::string(tr(lang, "ed.slot.sub_open")) + " (" + std::to_string(s->children.size()) + ")"),
                    false, !atMaxDepth, false, false, atMaxDepth,
                });
            } else if (s->type == ActionType::Macro) {
                int totalSteps = static_cast<int>(s->steps.size());
                int maxPage    = std::max(0, (totalSteps - 1) / 2);
                int page       = std::clamp(mMacroPage, 0, maxPage);
                int startStep  = page * 2;
                int endStep    = std::min(totalSteps, startStep + 2);

                for (int i = startStep; i < endStep; ++i) {
                    auto const& st = s->steps[static_cast<size_t>(i)];
                    char const* tg = (st.type == ActionType::Chat)          ? tr(lang, "ed.type.chat")
                                   : (st.type == ActionType::ClientCommand) ? tr(lang, "ed.type.cli")
                                                                            : tr(lang, "ed.type.cmd");
                    std::string lbl = std::to_string(i + 1) + ".[" + tg + "] " + st.value + " (+" + std::to_string(st.delayMs) + "ms)";
                    L.buttons.push_back(EditorButton{{rx, ry, rx + rw, ry + 11.5f}, "mstep:" + std::to_string(i), lbl, i == mSelectedMacroStep});
                    ry += 12.5f;
                }

                // Macro controls row: [<] [>] [+Step] [Type] [Up] [Del]
                float mbw = (rw - 10.0f) / 6.0f;
                float mx  = rx;
                L.buttons.push_back(EditorButton{{mx, ry, mx + mbw, ry + 11.5f}, "mstep_prev_page", "<", false, false, false, false, page <= 0});
                mx += mbw + 2.0f;
                L.buttons.push_back(EditorButton{{mx, ry, mx + mbw, ry + 11.5f}, "mstep_next_page", ">", false, false, false, false, page >= maxPage});
                mx += mbw + 2.0f;
                L.buttons.push_back(EditorButton{{mx, ry, mx + mbw, ry + 11.5f}, "mstep_add", tr(lang, "ed.macro.add"), false, true, false, false, totalSteps >= kMaxMacroSteps});
                mx += mbw + 2.0f;
                L.buttons.push_back(EditorButton{{mx, ry, mx + mbw, ry + 11.5f}, "mstep_type", tr(lang, "ed.macro.type")});
                mx += mbw + 2.0f;
                L.buttons.push_back(EditorButton{{mx, ry, mx + mbw, ry + 11.5f}, "mstep_up", tr(lang, "ed.macro.up"), false, false, false, false, mSelectedMacroStep <= 0});
                mx += mbw + 2.0f;
                L.buttons.push_back(EditorButton{{mx, ry, rx + rw, ry + 11.5f}, "mstep_del", tr(lang, "ed.macro.del"), false, false, true, false, totalSteps <= 1});
                ry += 13.5f;

                if (!s->steps.empty() && mSelectedMacroStep >= 0 && mSelectedMacroStep < totalSteps
                    && ry + 19.5f <= footerGuardY) {
                    auto const& st = s->steps[static_cast<size_t>(mSelectedMacroStep)];
                    pushTextBox({rx, ry + 7.5f, rx + rw, ry + 19.5f}, TextFieldId::MacroStepValue, tr(lang, "ed.macro.step_cap"), st.value, tr(lang, "hint.slot_cmd"));
                    ry += 21.5f;
                    if (ry + 11.5f <= footerGuardY) {
                        float bw3 = (rw - 4.0f) / 3.0f;
                        pushHintBtn(EditorButton{{rx, ry, rx + bw3, ry + 11.5f}, "mstep_delay_dec", "-100ms"}, tr(lang, "hint.macro_delay"));
                        EditorButton dBadge{{rx + bw3 + 2.0f, ry, rx + bw3 * 2.0f + 2.0f, ry + 11.5f}, "mstep_delay_noop", std::to_string(st.delayMs) + "ms"};
                        dBadge.isBadge = true;
                        dBadge.hint    = tr(lang, "hint.macro_delay");
                        L.buttons.push_back(std::move(dBadge));
                        pushHintBtn(EditorButton{{rx + (bw3 + 2.0f) * 2.0f, ry, rx + rw, ry + 11.5f}, "mstep_delay_inc", "+100ms"}, tr(lang, "hint.macro_delay"));
                    }
                }
            }

            // Bottom anchored 16-color palette (2 neat rows of 8) + Per-slot Cooldown with clear labels
            float swY1  = L.rightPane.y1 - 51.0f;
            float swH   = 10.5f;
            float swGap = 2.0f;
            L.labels.push_back({{rx, swY1 - 7.8f, rx + rw, swY1}, tr(lang, "ed.color.hdr"), false, false});

            constexpr int kCols = 8;
            for (int row = 0; row < 2; ++row) {
                float rowY  = swY1 + static_cast<float>(row) * (swH + swGap);
                float autoW = (row == 0) ? 24.0f : 0.0f;
                float swW   = (row == 0)
                                ? ((rw - autoW - static_cast<float>(kCols - 1) * 2.0f) / static_cast<float>(kCols - 1))
                                : ((rw - static_cast<float>(kCols - 1) * 2.0f) / static_cast<float>(kCols));
                float sx    = rx;
                for (int col = 0; col < kCols; ++col) {
                    int idx = row * kCols + col;
                    if (idx >= static_cast<int>(std::size(kColorPalette))) break;
                    float w     = (idx == 0) ? autoW : swW;
                    bool  isSel = (s->color == kColorPalette[idx].hex);
                    EditorButton sb{
                        {sx, rowY, sx + w, rowY + swH},
                        std::string("color:") + kColorPalette[idx].hex,
                        idx == 0 ? tr(lang, "ed.color.auto") : "",
                        isSel, false, false, false, false,
                        kColorPalette[idx].rgb,
                    };
                    sb.hint = tr(lang, "hint.color");
                    L.buttons.push_back(std::move(sb));
                    sx += w + 2.0f;
                }
            }

            float cdY = L.rightPane.y1 - 16.0f;
            L.labels.push_back({{rx, cdY - 7.8f, rx + rw, cdY}, tr(lang, "ed.cd.hdr"), false, false});
            float stepW = 28.0f;
            std::string cdTxt = (s->cooldownMs < 0) ? tr(lang, "ed.cd.global") : (std::to_string(s->cooldownMs) + "ms");
            pushHintBtn(EditorButton{{rx, cdY, rx + stepW, cdY + 12.5f}, "slot_cd_dec", "-"}, tr(lang, "hint.slot_cd"));
            pushHintBtn(EditorButton{{rx + stepW + 2.0f, cdY, rx + rw - stepW - 2.0f, cdY + 12.5f}, "slot_cd_reset", cdTxt}, tr(lang, "hint.slot_cd"));
            pushHintBtn(EditorButton{{rx + rw - stepW, cdY, rx + rw, cdY + 12.5f}, "slot_cd_inc", "+"}, tr(lang, "hint.slot_cd"));
        }
    }

    // =========================================================================
    // TAB 2: SERVER AUTO-SWITCH RULES (Multi-rule + Port + Priority + Live Match)
    // =========================================================================
    else if (mActiveTab == EditorTab::Servers) {
        float lx = L.leftPane.x0 + 5.0f;
        float lw = L.leftPane.width() - 10.0f;
        float ly = L.leftPane.y0 + 14.0f;

        if (mServer.inWorld) {
            pushHintBtn(EditorButton{{lx, ly, lx + lw, ly + 15.0f}, "new_for_server", tr(lang, "ed.srv.create_cur"), false, true}, tr(lang, "hint.srv_create"));
            ly += 18.0f;
        }

        float botY         = L.leftPane.y1 - 17.0f;
        int   availRows    = std::max(2, static_cast<int>((botY - ly - 16.0f) / 15.0f));
        int   totalPresets = static_cast<int>(mDraft.presets.size());
        int   maxShow      = std::min(availRows, 7);
        int   startIdx     = std::clamp(mSelectedPreset - maxShow / 2, 0, std::max(0, totalPresets - maxShow));
        int   endIdx       = std::min(totalPresets, startIdx + maxShow);

        for (int i = startIdx; i < endIdx; ++i) {
            auto const& p = mDraft.presets[static_cast<size_t>(i)];
            std::string lbl = std::to_string(i + 1) + ". " + p.name;
            if (!p.serverRules.empty()) {
                lbl += " (" + std::to_string(p.serverRules.size()) + ")";
            }
            L.buttons.push_back(EditorButton{{lx, ly, lx + lw, ly + 13.5f}, "preset:" + std::to_string(i), lbl, i == mSelectedPreset});
            ly += 15.0f;
        }

        if (totalPresets > maxShow && ly + 13.0f <= botY - 2.0f) {
            float hw = (lw - 3.0f) * 0.5f;
            L.buttons.push_back(EditorButton{{lx, ly, lx + hw, ly + 12.0f}, "preset_prev", "<"});
            L.buttons.push_back(EditorButton{{lx + hw + 3.0f, ly, lx + lw, ly + 12.0f}, "preset_next", ">"});
        }

        float crudW = (lw - 6.0f) / 3.0f;
        L.buttons.push_back(EditorButton{{lx, botY, lx + crudW, botY + 13.5f}, "preset_add", tr(lang, "ed.preset.new"), false, true});
        L.buttons.push_back(EditorButton{{lx + crudW + 3.0f, botY, lx + crudW * 2.0f + 3.0f, botY + 13.5f}, "preset_dup", tr(lang, "ed.preset.dup")});
        L.buttons.push_back(EditorButton{{lx + (crudW + 3.0f) * 2.0f, botY, lx + lw, botY + 13.5f}, "preset_del", tr(lang, "ed.preset.del"), false, false, true, false, totalPresets <= 1});

        // Right pane: Preset name + Multi-rule selector + Rule fields
        float rx = L.rightPane.x0 + 5.0f;
        float rw = L.rightPane.width() - 10.0f;
        float ry = L.rightPane.y0 + 14.0f;

        if (auto const* p = currentPreset()) {
            L.presetMatchesCurrent = evaluatePresetRules(*p, mServer).matched;
            L.totalRules           = static_cast<int>(p->serverRules.size());
            L.selectedRuleIndex    = (L.totalRules > 0) ? std::clamp(mSelectedRule, 0, L.totalRules - 1) : 0;

            pushTextBox({rx, ry + 7.5f, rx + rw, ry + 20.0f}, TextFieldId::PresetName, tr(lang, "ed.srv.preset_name"), p->name);
            ry += 22.5f;

            // Rule switcher row: [<] [Rule 1/2 badge] [>] [+Add] [Del]
            std::string rIdxTxt = (L.totalRules == 0)
                                    ? std::string(tr(lang, "ed.srv.rule_none"))
                                    : (std::string(tr(lang, "ed.srv.rule_prefix")) + std::to_string(L.selectedRuleIndex + 1) + "/" + std::to_string(L.totalRules));
            float arrowW = 16.0f;
            float addW   = 36.0f;
            float delW   = 30.0f;
            float midW   = std::max(38.0f, rw - (arrowW * 2.0f + addW + delW + 8.0f));
            float cx     = rx;
            pushHintBtn(EditorButton{{cx, ry, cx + arrowW, ry + 12.5f}, "rule_prev", "<", false, false, false, false, L.totalRules <= 1}, tr(lang, "hint.srv_rule_nav"));
            cx += arrowW + 2.0f;
            EditorButton rBadge{{cx, ry, cx + midW, ry + 12.5f}, "rule_noop", rIdxTxt};
            rBadge.isBadge = true;
            rBadge.hint    = tr(lang, "hint.srv_rule_nav");
            L.buttons.push_back(std::move(rBadge));
            cx += midW + 2.0f;
            pushHintBtn(EditorButton{{cx, ry, cx + arrowW, ry + 12.5f}, "rule_next", ">", false, false, false, false, L.totalRules <= 1}, tr(lang, "hint.srv_rule_nav"));
            cx += arrowW + 2.0f;
            pushHintBtn(EditorButton{{cx, ry, cx + addW, ry + 12.5f}, "rule_add", tr(lang, "ed.srv.rule_add"), false, true, false, false, L.totalRules >= kMaxRulesPerPreset}, tr(lang, "hint.srv_rule_nav"));
            cx += addW + 2.0f;
            pushHintBtn(EditorButton{{cx, ry, rx + rw, ry + 12.5f}, "rule_del", tr(lang, "ed.srv.rule_del"), false, false, true, false, L.totalRules == 0}, tr(lang, "hint.srv_rule_nav"));
            ry += 15.0f;

            ServerRule rule = (L.totalRules > 0) ? p->serverRules[static_cast<size_t>(L.selectedRuleIndex)] : ServerRule{};

            // Scope 3-way selector
            float sw3 = (rw - 4.0f) / 3.0f;
            pushHintBtn(EditorButton{{rx, ry, rx + sw3, ry + 12.5f}, "scope:any", tr(lang, "ed.srv.scope_any"), rule.scope == ConnectionScope::Any}, tr(lang, "hint.srv_scope"));
            pushHintBtn(EditorButton{{rx + sw3 + 2.0f, ry, rx + sw3 * 2.0f + 2.0f, ry + 12.5f}, "scope:external", tr(lang, "ed.srv.scope_ext"), rule.scope == ConnectionScope::External}, tr(lang, "hint.srv_scope"));
            pushHintBtn(EditorButton{{rx + (sw3 + 2.0f) * 2.0f, ry, rx + rw, ry + 12.5f}, "scope:local", tr(lang, "ed.srv.scope_loc"), rule.scope == ConnectionScope::Local}, tr(lang, "hint.srv_scope"));
            ry += 15.0f;

            float fillW = 34.0f;
            pushTextBox({rx, ry + 7.5f, rx + rw - fillW - 3.0f, ry + 20.0f}, TextFieldId::HostPattern, tr(lang, "ed.srv.host_pat"), rule.hostPattern, tr(lang, "hint.srv_host"));
            pushHintBtn(EditorButton{{rx + rw - fillW, ry + 7.5f, rx + rw, ry + 20.0f}, "fill_host", tr(lang, "ed.srv.fill"), false, true}, tr(lang, "hint.srv_fill"));
            ry += 22.5f;

            pushTextBox({rx, ry + 7.5f, rx + rw - fillW - 3.0f, ry + 20.0f}, TextFieldId::ServerNamePattern, tr(lang, "ed.srv.sname_pat"), rule.serverNamePattern, tr(lang, "hint.srv_sname"));
            pushHintBtn(EditorButton{{rx + rw - fillW, ry + 7.5f, rx + rw, ry + 20.0f}, "fill_sname", tr(lang, "ed.srv.fill"), false, true}, tr(lang, "hint.srv_fill"));
            ry += 22.5f;

            pushTextBox({rx, ry + 7.5f, rx + rw - fillW - 3.0f, ry + 20.0f}, TextFieldId::WorldNamePattern, tr(lang, "ed.srv.wname_pat"), rule.worldNamePattern, tr(lang, "hint.srv_wname"));
            pushHintBtn(EditorButton{{rx + rw - fillW, ry + 7.5f, rx + rw, ry + 20.0f}, "fill_wname", tr(lang, "ed.srv.fill"), false, true}, tr(lang, "hint.srv_fill"));
            ry += 23.0f;

            // Bottom row: Port box + Priority (with explicit caption & non-clickable value badge!)
            float botRowY = std::min(ry, L.rightPane.y1 - 22.0f);
            float portW   = std::floor(rw * 0.36f);
            pushTextBox({rx, botRowY + 7.5f, rx + portW, botRowY + 20.0f}, TextFieldId::RulePort, tr(lang, "ed.srv.port"), mPortEditBuf, tr(lang, "hint.srv_port"));

            float prX = rx + portW + 5.0f;
            float prW = rw - portW - 5.0f;
            float pbW = (prW - 4.0f) / 3.0f;
            L.labels.push_back({{prX, botRowY - 0.3f, rx + rw, botRowY + 7.5f}, tr(lang, "ed.srv.prio"), false, false});
            pushHintBtn(EditorButton{{prX, botRowY + 7.5f, prX + pbW, botRowY + 20.0f}, "prio_dec", "-10"}, tr(lang, "hint.srv_prio"));
            EditorButton pBadge{{prX + pbW + 2.0f, botRowY + 7.5f, prX + pbW * 2.0f + 2.0f, botRowY + 20.0f}, "prio_noop", std::to_string(rule.priority)};
            pBadge.isBadge = true;
            pBadge.hint    = tr(lang, "hint.srv_prio");
            L.buttons.push_back(std::move(pBadge));
            pushHintBtn(EditorButton{{prX + (pbW + 2.0f) * 2.0f, botRowY + 7.5f, rx + rw, botRowY + 20.0f}, "prio_inc", "+10"}, tr(lang, "hint.srv_prio"));
        }
    }

    // =========================================================================
    // TAB 3: CONTROLS & APPEARANCE (2-Row Key Chips + Paged Drag Sliders + Toggles)
    // =========================================================================
    else if (mActiveTab == EditorTab::Settings) {
        float lx = L.leftPane.x0 + 5.0f;
        float lw = L.leftPane.width() - 10.0f;
        float ly = L.leftPane.y0 + 14.0f;

        // Wheel activation key capture + 2-row wide chips
        std::string actLbl = (mCaptureTarget == CaptureTarget::ActivationKey)
                               ? std::string(tr(lang, "ed.set.press_key"))
                               : (std::string(tr(lang, "ed.set.hold_key")) + keyCodeToName(mDraft.activationKey));
        pushHintBtn(EditorButton{{lx, ly, lx + lw, ly + 14.0f}, "bind_activation", actLbl, mCaptureTarget == CaptureTarget::ActivationKey, true}, tr(lang, "hint.set_actkey"));
        ly += 16.0f;

        static constexpr char const* kQuickRow1[] = {"Alt", "Tab", "R", "V"};
        static constexpr char const* kQuickRow2[] = {"G", "Mouse4", "Mouse5"};
        float rw1 = (lw - 6.0f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            float qx = lx + static_cast<float>(i) * (rw1 + 2.0f);
            auto  vk = parseKeyName(kQuickRow1[i]);
            L.buttons.push_back(EditorButton{{qx, ly, qx + rw1, ly + 12.0f}, std::string("setact:") + kQuickRow1[i], kQuickRow1[i], vk && *vk == mDraft.activationKey});
        }
        ly += 14.0f;
        float rw2 = (lw - 4.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            float qx = lx + static_cast<float>(i) * (rw2 + 2.0f);
            auto  vk = parseKeyName(kQuickRow2[i]);
            L.buttons.push_back(EditorButton{{qx, ly, qx + rw2, ly + 12.0f}, std::string("setact:") + kQuickRow2[i], kQuickRow2[i], vk && *vk == mDraft.activationKey});
        }
        ly += 15.0f;

        // Editor hotkey + Hold/Toggle mode selector
        std::string edLbl = (mCaptureTarget == CaptureTarget::EditorKey)
                              ? std::string(tr(lang, "ed.set.press_key"))
                              : (std::string(tr(lang, "ed.set.editor_key")) + keyCodeToName(mDraft.editorKey));
        float edCapW = lw * 0.56f;
        pushHintBtn(EditorButton{{lx, ly, lx + edCapW, ly + 13.0f}, "bind_editor", edLbl, mCaptureTarget == CaptureTarget::EditorKey, true}, tr(lang, "hint.set_edkey"));
        float eqW = (lw - edCapW - 4.0f) * 0.5f;
        L.buttons.push_back(EditorButton{{lx + edCapW + 2.0f, ly, lx + edCapW + 2.0f + eqW, ly + 13.0f}, "seted:F12", "F12", mDraft.editorKey == kVkF12});
        L.buttons.push_back(EditorButton{{lx + edCapW + 4.0f + eqW, ly, lx + lw, ly + 13.0f}, "seted:None", "None", mDraft.editorKey == 0});
        ly += 15.5f;

        // Hold vs Toggle mode selector
        float modeW = (lw - 3.0f) * 0.5f;
        pushHintBtn(EditorButton{{lx, ly, lx + modeW, ly + 13.0f}, "mode:hold", tr(lang, "ed.set.mode_hold"), mDraft.activationMode == ActivationMode::Hold}, tr(lang, "hint.set_mode_hold"));
        pushHintBtn(EditorButton{{lx + modeW + 3.0f, ly, lx + lw, ly + 13.0f}, "mode:toggle", tr(lang, "ed.set.mode_toggle"), mDraft.activationMode == ActivationMode::Toggle}, tr(lang, "hint.set_mode_toggle"));
        ly += 16.0f;

        // Paged toggles
        struct TogSpec { char const* id; char const* key; bool val; };
        TogSpec const allTogs[] = {
            {"enabled",      "ed.set.en_mod",      mDraft.enabled},
            {"exec_release", "ed.set.en_release",  mDraft.executeOnRelease},
            {"exec_click",   "ed.set.en_click",    mDraft.executeOnLeftClick},
            {"auto_switch",  "ed.set.en_auto",     mDraft.autoSwitchPreset},
            {"toast",        "ed.set.en_toast",    mDraft.showPresetToast},
            {"num_hints",    "ed.set.en_hints",    mDraft.showNumberHints},
            {"inv_wheel",    "ed.set.en_invwheel", mDraft.invertWheelScroll},
        };
        float botY   = L.leftPane.y1 - 16.5f;
        int   perPg  = std::max(2, std::min(4, static_cast<int>((botY - ly - 2.0f) / 14.5f)));
        int   maxTPg = (static_cast<int>(std::size(allTogs)) - 1) / perPg;
        int   tPg    = std::clamp(mSettingsTogPage, 0, maxTPg);
        int   tBegin = tPg * perPg;
        int   tEnd   = std::min<int>(static_cast<int>(std::size(allTogs)), tBegin + perPg);
        for (int i = tBegin; i < tEnd; ++i) {
            L.toggles.push_back(EditorToggle{{lx, ly, lx + lw, ly + 13.0f}, allTogs[i].id, tr(lang, allTogs[i].key), allTogs[i].val});
            ly += 14.5f;
        }

        // Bottom left: localized toggle page button + Reset Defaults button
        float pgBtnW = std::floor(lw * 0.44f);
        L.buttons.push_back(EditorButton{
            {lx, botY, lx + pgBtnW, botY + 13.0f},
            "tog_page",
            std::string(tr(lang, "ed.set.tog_next")) + std::to_string(tPg + 1) + "/" + std::to_string(maxTPg + 1)
        });
        L.buttons.push_back(EditorButton{{lx + pgBtnW + 4.0f, botY, lx + lw, botY + 13.0f}, "reset_defaults",
                                         tr(lang, mConfirmReset ? "ed.reset.confirm" : "ed.reset.btn"), false, false, true});

        // Right pane: Paged Click/Drag Visual Sliders
        float rx = L.rightPane.x0 + 6.0f;
        float rw = L.rightPane.width() - 12.0f;
        float ry = L.rightPane.y0 + 16.0f;

        struct SlSpec { std::string id; std::string lbl; std::string val; float ratio; };
        std::vector<SlSpec> allSliders = {
            {"sens",       tr(lang, "ed.set.sl.sens"),    fmtFloat(mDraft.flickSensitivity, 2) + "x",                   normRatio(mDraft.flickSensitivity, 0.2f, 4.0f)},
            {"outer_r",    tr(lang, "ed.set.sl.outer"),   fmtFloat(mDraft.outerRadius, 0) + "px",                       normRatio(mDraft.outerRadius, 50.0f, 180.0f)},
            {"inner_r",    tr(lang, "ed.set.sl.inner"),   fmtFloat(mDraft.innerRadius, 0) + "px",                       normRatio(mDraft.innerRadius, 14.0f, 85.0f)},
            {"deadzone",   tr(lang, "ed.set.sl.dead"),    fmtFloat(mDraft.deadzoneRadius, 0) + "px",                    normRatio(mDraft.deadzoneRadius, 8.0f, 80.0f)},
            {"opacity",    tr(lang, "ed.set.sl.opacity"), fmtFloat(mDraft.backgroundOpacity * 100.0f, 0) + "%",         normRatio(mDraft.backgroundOpacity, 0.15f, 1.0f)},
            {"cooldown",   tr(lang, "ed.set.sl.cd"),      std::to_string(mDraft.globalCooldownMs) + "ms",               normRatio(static_cast<float>(mDraft.globalCooldownMs), 0.0f, 3000.0f)},
            {"sub_hover",  tr(lang, "ed.set.sl.sub"),     mDraft.submenuHoverMs == 0 ? "OFF" : (std::to_string(mDraft.submenuHoverMs) + "ms"), normRatio(static_cast<float>(mDraft.submenuHoverMs), 0.0f, 1000.0f)},
            {"hold_del",   tr(lang, "ed.set.sl.hold"),    std::to_string(mDraft.holdDelayMs) + "ms",                    normRatio(static_cast<float>(mDraft.holdDelayMs), 0.0f, 500.0f)},
            {"font_scale", tr(lang, "ed.set.sl.font"),    fmtFloat(mDraft.fontScale, 2) + "x",                          normRatio(mDraft.fontScale, 0.5f, 1.8f)},
        };

        float rBotY   = L.rightPane.y1 - 16.5f;
        int   slPerPg = std::max(3, std::min(5, static_cast<int>((rBotY - ry - 2.0f) / 24.0f)));
        int   maxSPg  = (static_cast<int>(allSliders.size()) - 1) / slPerPg;
        int   sPg     = std::clamp(mSettingsSlidePage, 0, maxSPg);
        int   sBegin  = sPg * slPerPg;
        int   sEnd    = std::min<int>(static_cast<int>(allSliders.size()), sBegin + slPerPg);

        for (int i = sBegin; i < sEnd; ++i) {
            auto const& sl = allSliders[static_cast<size_t>(i)];
            L.sliders.push_back(EditorSlider{{rx, ry + 8.5f, rx + rw, ry + 18.5f}, sl.id, sl.lbl, sl.val, sl.ratio, tr(lang, "hint.sl_drag")});
            ry += 24.0f;
        }

        if (maxSPg > 0) {
            float navW = std::floor((rw - 36.0f) * 0.5f);
            L.buttons.push_back(EditorButton{{rx, rBotY, rx + navW, rBotY + 13.0f}, "sl_prev", tr(lang, "ed.set.page_prev"), false, false, false, false, sPg <= 0});
            EditorButton pgBadge{{rx + navW + 3.0f, rBotY, rx + rw - navW - 3.0f, rBotY + 13.0f}, "sl_noop", std::to_string(sPg + 1) + " / " + std::to_string(maxSPg + 1)};
            pgBadge.isBadge = true;
            L.buttons.push_back(std::move(pgBadge));
            L.buttons.push_back(EditorButton{{rx + rw - navW, rBotY, rx + rw, rBotY + 13.0f}, "sl_next", tr(lang, "ed.set.page_next"), false, false, false, false, sPg >= maxSPg});
        }
    }

    return L;
}

void EditorModel::applySliderRatio(std::string_view id, float ratio, bool recordUndo) {
    if (recordUndo) pushUndoSnapshot();
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    if (id == "sens") {
        mDraft.flickSensitivity = std::round((0.2f + ratio * 3.8f) * 20.0f) / 20.0f;
    } else if (id == "outer_r") {
        mDraft.outerRadius = std::round(50.0f + ratio * 130.0f);
    } else if (id == "inner_r") {
        mDraft.innerRadius = std::round(14.0f + ratio * 71.0f);
    } else if (id == "deadzone") {
        mDraft.deadzoneRadius = std::round(8.0f + ratio * 72.0f);
    } else if (id == "opacity") {
        mDraft.backgroundOpacity = std::round((0.15f + ratio * 0.85f) * 20.0f) / 20.0f;
    } else if (id == "cooldown") {
        mDraft.globalCooldownMs = static_cast<int>(std::lround((ratio * 3000.0f) / 50.0f)) * 50;
    } else if (id == "sub_hover") {
        mDraft.submenuHoverMs = static_cast<int>(std::lround((ratio * 1000.0f) / 50.0f)) * 50;
    } else if (id == "hold_del") {
        mDraft.holdDelayMs = static_cast<int>(std::lround((ratio * 500.0f) / 25.0f)) * 25;
    } else if (id == "font_scale") {
        mDraft.fontScale = std::round((0.5f + ratio * 1.3f) * 20.0f) / 20.0f;
    }
    sanitizeConfig(mDraft);
    mDirty = true;
}

void EditorModel::applyToggle(std::string_view id) {
    pushUndoSnapshot();
    if (id == "enabled") mDraft.enabled = !mDraft.enabled;
    else if (id == "exec_release") mDraft.executeOnRelease = !mDraft.executeOnRelease;
    else if (id == "exec_click") mDraft.executeOnLeftClick = !mDraft.executeOnLeftClick;
    else if (id == "auto_switch") mDraft.autoSwitchPreset = !mDraft.autoSwitchPreset;
    else if (id == "toast") mDraft.showPresetToast = !mDraft.showPresetToast;
    else if (id == "num_hints") mDraft.showNumberHints = !mDraft.showNumberHints;
    else if (id == "inv_wheel") mDraft.invertWheelScroll = !mDraft.invertWheelScroll;
    sanitizeConfig(mDraft);
    mDirty = true;
}

void EditorModel::focusField(TextFieldId id, bool moveCursorToEnd) {
    mFocusedField = id;
    mSelectedAll  = false;
    if (moveCursorToEnd) {
        mCursorByte = getFieldValue(id).size();
    } else {
        mCursorByte = std::min(mCursorByte, getFieldValue(id).size());
    }
}

void EditorModel::cycleFocusField(bool backwards) {
    std::vector<TextFieldId> order;
    if (mActiveTab == EditorTab::Wheel) {
        if (auto const* s = currentSlot()) {
            order.push_back(TextFieldId::SlotLabel);
            if (s->type == ActionType::Command || s->type == ActionType::Chat || s->type == ActionType::ClientCommand) {
                order.push_back(TextFieldId::SlotValue);
                order.push_back(TextFieldId::SlotSubtitle);
            } else if (s->type == ActionType::Macro && !s->steps.empty()) {
                order.push_back(TextFieldId::MacroStepValue);
            }
        }
    } else if (mActiveTab == EditorTab::Servers) {
        order = {
            TextFieldId::PresetName,
            TextFieldId::HostPattern,
            TextFieldId::ServerNamePattern,
            TextFieldId::WorldNamePattern,
            TextFieldId::RulePort,
        };
    }
    if (order.empty()) return;

    auto it = std::find(order.begin(), order.end(), mFocusedField);
    if (it == order.end()) {
        focusField(backwards ? order.back() : order.front(), true);
        return;
    }
    size_t idx = static_cast<size_t>(it - order.begin());
    size_t nxt = backwards ? ((idx + order.size() - 1) % order.size()) : ((idx + 1) % order.size());
    focusField(order[nxt], true);
}

std::string EditorModel::hoverHintAt(EditorLayout const& layout, float mx, float my) {
    for (auto const& b : layout.buttons) {
        if (!b.hint.empty() && b.rect.contains(mx, my)) return b.hint;
    }
    for (auto const& tb : layout.textBoxes) {
        if (!tb.hint.empty() && tb.rect.contains(mx, my)) return tb.hint;
    }
    for (auto const& sl : layout.sliders) {
        geom::RectF r{sl.rect.x0 - 2.0f, sl.rect.y0 - 9.0f, sl.rect.x1 + 2.0f, sl.rect.y1 + 2.0f};
        if (!sl.hint.empty() && r.contains(mx, my)) return sl.hint;
    }
    for (auto const& tg : layout.toggles) {
        if (!tg.hint.empty() && tg.rect.contains(mx, my)) return tg.hint;
    }
    if (layout.wheelPreview.visible && layout.wheelPreview.bounds.contains(mx, my)) {
        return tr(layout.lang, "hint.wheel_preview");
    }
    return {};
}

EditorCommand EditorModel::requestCloseWithConfirm() {
    mFocusedField  = TextFieldId::None;
    mSelectedAll   = false;
    mCaptureTarget = CaptureTarget::None;
    if (mDirty && !mConfirmClose) {
        mConfirmClose = true;
        return EditorCommand::None;
    }
    return EditorCommand::Close;
}

bool EditorModel::handleDrag(float mx, float /*my*/, float screenW, float screenH) {
    if (mDraggingSliderId.empty()) return false;
    auto L = buildLayout(screenW, screenH);
    for (auto const& sl : L.sliders) {
        if (sl.id == mDraggingSliderId) {
            float r = (sl.rect.width() > 1.0f) ? ((mx - sl.rect.x0) / sl.rect.width()) : 0.0f;
            applySliderRatio(sl.id, r, false);
            return true;
        }
    }
    return false;
}

bool EditorModel::handleMouseWheel(int delta) {
    if (delta == 0) return false;
    if (mActiveTab == EditorTab::Settings) {
        mSettingsSlidePage = std::clamp(mSettingsSlidePage + (delta > 0 ? -1 : 1), 0, 2);
        return true;
    }
    if (mActiveTab == EditorTab::Servers && mDraft.presets.size() > 1) {
        int n = static_cast<int>(mDraft.presets.size());
        mSelectedPreset = (mSelectedPreset + (delta > 0 ? -1 : 1) + n) % n;
        mSelectedRule   = 0;
        mSubmenuPath.clear();
        mSelectedSlot = 0;
        ensureValidSelection();
        return true;
    }
    return false;
}

EditorCommand EditorModel::handleClick(
    float                     mx,
    float                     my,
    float                     screenW,
    float                     screenH,
    TextWidthMeasureFn const& inputMeasure,
    bool                      isDoubleClick
) {
    mCaptureTarget = CaptureTarget::None;
    mDraggingSliderId.clear();
    auto L = buildLayout(screenW, screenH);

    // 1) Interactive circular wheel preview click
    if (L.wheelPreview.visible && L.wheelPreview.bounds.contains(mx, my)) {
        mConfirmReset = false;
        int hit       = hitTestWheelPreview(L.wheelPreview, mx, my);
        if (hit == -2) {
            if (!mSubmenuPath.empty()) {
                mSelectedSlot = mSubmenuPath.back();
                mSubmenuPath.pop_back();
                mFocusedField = TextFieldId::None;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (hit >= 0) {
            mFocusedField = TextFieldId::None;
            if (hit == mSelectedSlot) {
                if (auto* s = currentSlot(); s && s->type == ActionType::Submenu
                    && static_cast<int>(mSubmenuPath.size()) < kMaxSubmenuDepth) {
                    if (s->children.empty()) {
                        pushUndoSnapshot();
                        s->children.push_back(RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#34D399"});
                        s->children.push_back(RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"});
                        mDirty = true;
                    }
                    mSubmenuPath.push_back(mSelectedSlot);
                    mSelectedSlot = 0;
                    ensureValidSelection();
                    return EditorCommand::None;
                }
            }
            mSelectedSlot      = hit;
            mSelectedMacroStep = 0;
            mMacroPage         = 0;
            ensureValidSelection();
            return EditorCommand::None;
        }
    }

    // 2) Clickable / draggable sliders
    for (auto const& sl : L.sliders) {
        geom::RectF hitR{sl.rect.x0 - 2.0f, sl.rect.y0 - 2.0f, sl.rect.x1 + 2.0f, sl.rect.y1 + 2.0f};
        if (hitR.contains(mx, my)) {
            mFocusedField     = TextFieldId::None;
            mConfirmReset     = false;
            mDraggingSliderId = sl.id;
            float r           = (sl.rect.width() > 1.0f) ? ((mx - sl.rect.x0) / sl.rect.width()) : 0.0f;
            applySliderRatio(sl.id, r, true);
            return EditorCommand::None;
        }
    }

    // 3) Toggles
    for (auto const& tg : L.toggles) {
        if (tg.rect.contains(mx, my)) {
            mFocusedField = TextFieldId::None;
            mConfirmReset = false;
            applyToggle(tg.id);
            return EditorCommand::None;
        }
    }

    // 4) Text boxes (click-to-caret, double-click select-all, right-edge [x] clear button)
    for (auto const& tb : L.textBoxes) {
        if (!tb.value.empty() && tb.clearRect.width() > 1.0f && tb.clearRect.contains(mx, my)) {
            pushUndoSnapshot();
            setFieldValue(tb.field, "");
            focusField(tb.field, true);
            mDirty = true;
            return EditorCommand::None;
        }
        if (tb.rect.contains(mx, my)) {
            mConfirmReset = false;
            bool wasFocused = (mFocusedField == tb.field);
            if (wasFocused && isDoubleClick && !tb.value.empty()) {
                mSelectedAll = true;
                mCursorByte  = tb.value.size();
                return EditorCommand::None;
            }
            float const textLeft  = tb.rect.x0 + 3.5f;
            float const textRight = tb.clearRect.width() > 1.0f ? (tb.clearRect.x0 - 2.0f) : (tb.rect.x1 - 3.0f);
            float const availW    = std::max(4.0f, textRight - textLeft);
            size_t const prevCur  = wasFocused ? std::min(mCursorByte, tb.value.size()) : 0;
            size_t const newCur   = caretByteFromClickOffset(tb.value, prevCur, mx - textLeft, availW, 0.68f, inputMeasure);
            mFocusedField = tb.field;
            mSelectedAll  = false;
            mCursorByte   = newCur;
            return EditorCommand::None;
        }
    }

    // 5) Buttons
    for (auto const& b : L.buttons) {
        if (!b.rect.contains(mx, my)) continue;
        if (b.disabled || b.isBadge) return EditorCommand::None;
        mFocusedField = TextFieldId::None;
        if (b.id != "reset_defaults") mConfirmReset = false;

        if (b.id == "save") return EditorCommand::Save;
        if (b.id == "reload") { mConfirmClose = false; return EditorCommand::Reload; }
        if (b.id == "undo") { undo(); return EditorCommand::None; }
        if (b.id == "close") {
            if (mDirty && !mConfirmClose) {
                mConfirmClose = true;
                return EditorCommand::None;
            }
            return EditorCommand::Close;
        }
        if (b.id == "confirm_stay") { mConfirmClose = false; return EditorCommand::None; }
        if (b.id == "confirm_discard") { mConfirmClose = false; return EditorCommand::Close; }
        if (b.id == "confirm_save_close") { mConfirmClose = false; return EditorCommand::SaveAndClose; }
        if (b.id == "lang_toggle") {
            pushUndoSnapshot();
            mDraft.language = (resolvedLang() == ResolvedLang::Ja) ? UiLanguage::En : UiLanguage::Ja;
            mDirty          = true;
            mStatus         = tr(resolvedLang(), "ed.status.welcome");
            return EditorCommand::None;
        }

        if (b.id == "tab:wheel") { mActiveTab = EditorTab::Wheel; return EditorCommand::None; }
        if (b.id == "tab:servers") { mActiveTab = EditorTab::Servers; return EditorCommand::None; }
        if (b.id == "tab:settings") { mActiveTab = EditorTab::Settings; return EditorCommand::None; }

        if (b.id == "slot_test") {
            mStatus = tr(resolvedLang(), "ed.status.tested");
            return EditorCommand::TestSlot;
        }

        if (b.id == "new_for_server") { createPresetForCurrentServer(); return EditorCommand::None; }
        if (b.id == "bind_activation") { mCaptureTarget = CaptureTarget::ActivationKey; return EditorCommand::None; }
        if (b.id == "bind_editor") { mCaptureTarget = CaptureTarget::EditorKey; return EditorCommand::None; }
        if (b.id.starts_with("setact:")) {
            if (auto vk = parseKeyName(b.id.substr(7))) {
                pushUndoSnapshot();
                mDraft.activationKey = *vk;
                mDirty               = true;
            }
            return EditorCommand::None;
        }
        if (b.id.starts_with("seted:")) {
            if (auto vk = parseKeyName(b.id.substr(6))) {
                pushUndoSnapshot();
                mDraft.editorKey = *vk;
                mDirty           = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "mode:hold" || b.id == "mode:toggle") {
            pushUndoSnapshot();
            mDraft.activationMode = (b.id == "mode:toggle") ? ActivationMode::Toggle : ActivationMode::Hold;
            mDirty                = true;
            return EditorCommand::None;
        }
        if (b.id == "tog_page") {
            mSettingsTogPage = (mSettingsTogPage + 1) % 3;
            return EditorCommand::None;
        }
        if (b.id == "sl_prev") { mSettingsSlidePage = std::max(0, mSettingsSlidePage - 1); return EditorCommand::None; }
        if (b.id == "sl_next") { ++mSettingsSlidePage; return EditorCommand::None; }
        if (b.id == "reset_defaults") {
            if (!mConfirmReset) {
                mConfirmReset = true;
                return EditorCommand::None;
            }
            pushUndoSnapshot();
            UiLanguage keepLang = mDraft.language;
            mDraft              = makeDefaultConfig();
            mDraft.language     = keepLang;
            mConfirmReset       = false;
            mDirty              = true;
            mSubmenuPath.clear();
            mSelectedPreset = 0;
            mSelectedSlot   = 0;
            ensureValidSelection();
            mStatus = tr(resolvedLang(), "ed.status.reset");
            return EditorCommand::None;
        }

        if (b.id.starts_with("preset:")) {
            mSelectedPreset = std::atoi(b.id.c_str() + 7);
            mSelectedRule   = 0;
            mSubmenuPath.clear();
            mSelectedSlot = 0;
            ensureValidSelection();
            return EditorCommand::None;
        }
        if (b.id == "preset_prev" || b.id == "preset_next") {
            int n = static_cast<int>(mDraft.presets.size());
            if (n > 1) {
                mSelectedPreset = (mSelectedPreset + (b.id == "preset_next" ? 1 : -1) + n) % n;
                mSelectedRule   = 0;
                mSubmenuPath.clear();
                mSelectedSlot = 0;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id == "preset_add") {
            if (static_cast<int>(mDraft.presets.size()) < kMaxPresets) {
                pushUndoSnapshot();
                RadialPreset np;
                np.name = "Preset " + std::to_string(mDraft.presets.size() + 1);
                np.id   = makeUniquePresetId(mDraft, np.name);
                np.slots = {
                    RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"},
                    RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#34D399"},
                    RadialSlot{"Spawn", "/spawn", ActionType::Command, "/spawn", "#60A5FA"},
                    RadialSlot{"Thanks", "Chat", ActionType::Chat, "ありがとう！", "#F59E0B"},
                };
                mDraft.presets.push_back(std::move(np));
                mSelectedPreset = static_cast<int>(mDraft.presets.size()) - 1;
                mSelectedRule   = 0;
                mSubmenuPath.clear();
                mSelectedSlot = 0;
                mDirty        = true;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id == "preset_dup") {
            if (auto const* p = currentPreset(); p && static_cast<int>(mDraft.presets.size()) < kMaxPresets) {
                pushUndoSnapshot();
                RadialPreset copy = *p;
                copy.name += " Copy";
                copy.id = makeUniquePresetId(mDraft, copy.name);
                mDraft.presets.push_back(std::move(copy));
                mSelectedPreset = static_cast<int>(mDraft.presets.size()) - 1;
                mSelectedRule   = 0;
                mSubmenuPath.clear();
                mSelectedSlot = 0;
                mDirty        = true;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id == "preset_del") {
            if (mDraft.presets.size() > 1 && mSelectedPreset >= 0
                && mSelectedPreset < static_cast<int>(mDraft.presets.size())) {
                pushUndoSnapshot();
                mDraft.presets.erase(mDraft.presets.begin() + mSelectedPreset);
                mSelectedRule = 0;
                mSubmenuPath.clear();
                mDirty = true;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }

        // Multi-rule server editing
        if (b.id == "rule_prev" || b.id == "rule_next") {
            if (auto* p = currentPreset(); p && p->serverRules.size() > 1) {
                int n         = static_cast<int>(p->serverRules.size());
                mSelectedRule = (mSelectedRule + (b.id == "rule_next" ? 1 : -1) + n) % n;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id == "rule_add") {
            if (auto* p = currentPreset(); p && static_cast<int>(p->serverRules.size()) < kMaxRulesPerPreset) {
                pushUndoSnapshot();
                p->serverRules.push_back(ServerRule{});
                mSelectedRule = static_cast<int>(p->serverRules.size()) - 1;
                mDirty        = true;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id == "rule_del") {
            if (auto* p = currentPreset(); p && !p->serverRules.empty()) {
                pushUndoSnapshot();
                int idx = std::clamp(mSelectedRule, 0, static_cast<int>(p->serverRules.size()) - 1);
                p->serverRules.erase(p->serverRules.begin() + idx);
                mDirty = true;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id.starts_with("scope:")) {
            if (auto* p = currentPreset()) {
                if (auto sc = parseConnectionScope(b.id.substr(6))) {
                    pushUndoSnapshot();
                    ensureSelectedRule(*p);
                    p->serverRules[static_cast<size_t>(mSelectedRule)].scope = *sc;
                    mDirty = true;
                }
            }
            return EditorCommand::None;
        }
        if (b.id == "fill_host") {
            if (auto* p = currentPreset()) {
                std::string sug = suggestHostPattern(mServer);
                if (!sug.empty()) {
                    pushUndoSnapshot();
                    ensureSelectedRule(*p);
                    auto& r       = p->serverRules[static_cast<size_t>(mSelectedRule)];
                    r.hostPattern = sug;
                    r.scope       = ConnectionScope::External;
                    mDirty        = true;
                }
            }
            return EditorCommand::None;
        }
        if (b.id == "fill_sname") {
            if (auto* p = currentPreset(); p && !mServer.serverName.empty()) {
                pushUndoSnapshot();
                ensureSelectedRule(*p);
                auto& r             = p->serverRules[static_cast<size_t>(mSelectedRule)];
                r.serverNamePattern = "*" + mServer.serverName + "*";
                r.scope             = ConnectionScope::External;
                mDirty              = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "fill_wname") {
            if (auto* p = currentPreset(); p && !mServer.worldName.empty()) {
                pushUndoSnapshot();
                ensureSelectedRule(*p);
                auto& r            = p->serverRules[static_cast<size_t>(mSelectedRule)];
                r.worldNamePattern = "*" + mServer.worldName + "*";
                r.scope            = ConnectionScope::Local;
                mDirty             = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "prio_dec" || b.id == "prio_inc") {
            if (auto* p = currentPreset()) {
                pushUndoSnapshot();
                ensureSelectedRule(*p);
                auto& r    = p->serverRules[static_cast<size_t>(mSelectedRule)];
                r.priority = std::clamp(r.priority + (b.id == "prio_inc" ? 10 : -10), -1000, 1000);
                mDirty     = true;
            }
            return EditorCommand::None;
        }

        // Submenu navigation
        if (b.id == "submenu_up") {
            if (!mSubmenuPath.empty()) {
                mSelectedSlot = mSubmenuPath.back();
                mSubmenuPath.pop_back();
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id == "submenu_open") {
            if (auto* s = currentSlot(); s && s->type == ActionType::Submenu
                && static_cast<int>(mSubmenuPath.size()) < kMaxSubmenuDepth) {
                if (s->children.empty()) {
                    pushUndoSnapshot();
                    s->children.push_back(RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#34D399"});
                    s->children.push_back(RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"});
                    mDirty = true;
                }
                mSubmenuPath.push_back(mSelectedSlot);
                mSelectedSlot = 0;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }

        // Slot list mutations
        if (b.id == "slot_add") {
            if (auto* list = currentSlotList(); list && static_cast<int>(list->size()) < kMaxSlotsPerMenu) {
                pushUndoSnapshot();
                list->push_back(RadialSlot{"Slot " + std::to_string(list->size() + 1), "", ActionType::Command, "/hub", ""});
                mSelectedSlot = static_cast<int>(list->size()) - 1;
                mDirty        = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "slot_dup") {
            if (auto* list = currentSlotList(); list && static_cast<int>(list->size()) < kMaxSlotsPerMenu
                && mSelectedSlot >= 0 && mSelectedSlot < static_cast<int>(list->size())) {
                pushUndoSnapshot();
                RadialSlot copy = (*list)[static_cast<size_t>(mSelectedSlot)];
                list->insert(list->begin() + mSelectedSlot + 1, std::move(copy));
                ++mSelectedSlot;
                mDirty = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "slot_up") {
            if (auto* list = currentSlotList(); list && list->size() > 1 && mSelectedSlot >= 0) {
                pushUndoSnapshot();
                int prev = (mSelectedSlot - 1 + static_cast<int>(list->size())) % static_cast<int>(list->size());
                std::swap((*list)[static_cast<size_t>(mSelectedSlot)], (*list)[static_cast<size_t>(prev)]);
                mSelectedSlot = prev;
                mDirty        = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "slot_down") {
            if (auto* list = currentSlotList(); list && list->size() > 1 && mSelectedSlot >= 0) {
                pushUndoSnapshot();
                int next = (mSelectedSlot + 1) % static_cast<int>(list->size());
                std::swap((*list)[static_cast<size_t>(mSelectedSlot)], (*list)[static_cast<size_t>(next)]);
                mSelectedSlot = next;
                mDirty        = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "slot_del") {
            if (auto* list = currentSlotList(); list && list->size() > static_cast<size_t>(kMinSlotsPerMenu)
                && mSelectedSlot >= 0 && mSelectedSlot < static_cast<int>(list->size())) {
                pushUndoSnapshot();
                list->erase(list->begin() + mSelectedSlot);
                mDirty = true;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }

        if (b.id.starts_with("type:")) {
            if (auto* s = currentSlot()) {
                if (auto parsed = parseActionType(b.id.substr(5))) {
                    pushUndoSnapshot();
                    s->type = *parsed;
                    if (s->type == ActionType::Macro && s->steps.empty()) {
                        s->steps.push_back(MacroStep{ActionType::Chat, "gg", 0});
                        s->steps.push_back(MacroStep{ActionType::Command, "/hub", 250});
                    }
                    if (s->type == ActionType::Submenu && s->children.empty()) {
                        s->children.push_back(RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#34D399"});
                        s->children.push_back(RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"});
                    }
                    mDirty = true;
                }
            }
            return EditorCommand::None;
        }

        if (b.id.starts_with("qval:")) {
            if (auto* s = currentSlot()) {
                pushUndoSnapshot();
                s->value = b.id.substr(5);
                mDirty   = true;
            }
            return EditorCommand::None;
        }

        if (b.id.starts_with("color:")) {
            if (auto* s = currentSlot()) {
                pushUndoSnapshot();
                s->color = b.id.substr(6);
                mDirty   = true;
            }
            return EditorCommand::None;
        }

        if (b.id == "slot_cd_dec" || b.id == "slot_cd_inc" || b.id == "slot_cd_reset") {
            if (auto* s = currentSlot()) {
                pushUndoSnapshot();
                if (b.id == "slot_cd_reset") s->cooldownMs = -1;
                else if (b.id == "slot_cd_dec") s->cooldownMs = (s->cooldownMs < 0) ? std::max(0, mDraft.globalCooldownMs - 100) : std::max(0, s->cooldownMs - 100);
                else s->cooldownMs = (s->cooldownMs < 0) ? (mDraft.globalCooldownMs + 100) : std::min(60000, s->cooldownMs + 100);
                mDirty = true;
            }
            return EditorCommand::None;
        }

        // Macro step controls
        if (b.id.starts_with("mstep:")) {
            mSelectedMacroStep = std::atoi(b.id.c_str() + 6);
            ensureValidSelection();
            return EditorCommand::None;
        }
        if (b.id == "mstep_prev_page") { mMacroPage = std::max(0, mMacroPage - 1); return EditorCommand::None; }
        if (b.id == "mstep_next_page") { ++mMacroPage; ensureValidSelection(); return EditorCommand::None; }
        if (b.id == "mstep_add") {
            if (auto* s = currentSlot(); s && s->type == ActionType::Macro
                && static_cast<int>(s->steps.size()) < kMaxMacroSteps) {
                pushUndoSnapshot();
                s->steps.push_back(MacroStep{ActionType::Command, "/hub", 200});
                mSelectedMacroStep = static_cast<int>(s->steps.size()) - 1;
                mMacroPage         = mSelectedMacroStep / 2;
                mDirty             = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "mstep_up") {
            if (auto* s = currentSlot(); s && s->type == ActionType::Macro && mSelectedMacroStep > 0
                && mSelectedMacroStep < static_cast<int>(s->steps.size())) {
                pushUndoSnapshot();
                std::swap(s->steps[static_cast<size_t>(mSelectedMacroStep)], s->steps[static_cast<size_t>(mSelectedMacroStep - 1)]);
                --mSelectedMacroStep;
                mMacroPage = mSelectedMacroStep / 2;
                mDirty     = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "mstep_del") {
            if (auto* s = currentSlot(); s && s->type == ActionType::Macro && s->steps.size() > 1
                && mSelectedMacroStep >= 0 && mSelectedMacroStep < static_cast<int>(s->steps.size())) {
                pushUndoSnapshot();
                s->steps.erase(s->steps.begin() + mSelectedMacroStep);
                mDirty = true;
                ensureValidSelection();
            }
            return EditorCommand::None;
        }
        if (b.id == "mstep_type") {
            if (auto* s = currentSlot(); s && s->type == ActionType::Macro && mSelectedMacroStep >= 0
                && mSelectedMacroStep < static_cast<int>(s->steps.size())) {
                pushUndoSnapshot();
                auto& st = s->steps[static_cast<size_t>(mSelectedMacroStep)];
                st.type  = (st.type == ActionType::Command) ? ActionType::Chat
                         : (st.type == ActionType::Chat)    ? ActionType::ClientCommand
                                                            : ActionType::Command;
                mDirty   = true;
            }
            return EditorCommand::None;
        }
        if (b.id == "mstep_delay_dec" || b.id == "mstep_delay_inc") {
            if (auto* s = currentSlot(); s && s->type == ActionType::Macro && mSelectedMacroStep >= 0
                && mSelectedMacroStep < static_cast<int>(s->steps.size())) {
                pushUndoSnapshot();
                auto& st   = s->steps[static_cast<size_t>(mSelectedMacroStep)];
                st.delayMs = std::clamp(st.delayMs + (b.id == "mstep_delay_inc" ? 100 : -100), 0, 10000);
                mDirty     = true;
            }
            return EditorCommand::None;
        }
    }

    mFocusedField = TextFieldId::None;
    return EditorCommand::None;
}

bool EditorModel::handleCapturedKey(int vk) {
    if (mCaptureTarget == CaptureTarget::None) return false;
    if (vk == kVkEscape) {
        mCaptureTarget = CaptureTarget::None;
        return true;
    }
    if (vk <= 0 || vk >= 255) return false;
    pushUndoSnapshot();
    int normalized = (vk == kVkLMenu || vk == kVkRMenu) ? kVkMenu : vk;
    if (mCaptureTarget == CaptureTarget::ActivationKey) {
        mDraft.activationKey = normalized;
    } else if (mCaptureTarget == CaptureTarget::EditorKey) {
        mDraft.editorKey = normalized;
    }
    mCaptureTarget = CaptureTarget::None;
    mDirty         = true;
    return true;
}

bool EditorModel::handleMouseButtonForCapture(int mouseVk) {
    if (mCaptureTarget == CaptureTarget::None || !isMouseButtonVk(mouseVk)) return false;
    return handleCapturedKey(mouseVk);
}

EditorCommand EditorModel::handleKey(
    int              vk,
    bool             shift,
    bool             ctrl,
    std::string_view clipboardUtf8,
    std::string_view typedUtf8,
    std::string*     outCopyUtf8
) {
    if (mCaptureTarget != CaptureTarget::None) {
        handleCapturedKey(vk);
        return EditorCommand::None;
    }

    if (vk == kVkEscape) {
        if (mFocusedField != TextFieldId::None) {
            mFocusedField = TextFieldId::None;
            mSelectedAll  = false;
            return EditorCommand::None;
        }
        if (mConfirmClose) {
            mConfirmClose = false;
            return EditorCommand::None;
        }
        return requestCloseWithConfirm();
    }

    if (mDraft.editorKey > 0 && keyMatchesBinding(vk, mDraft.editorKey)) {
        return requestCloseWithConfirm();
    }

    if (vk == kVkTab && !ctrl) {
        cycleFocusField(shift);
        return EditorCommand::None;
    }

    if (ctrl && (vk == 'S' || vk == 's')) {
        return EditorCommand::Save;
    }
    if (ctrl && (vk == 'Z' || vk == 'z')) {
        undo();
        return EditorCommand::None;
    }

    if (mFocusedField == TextFieldId::None) return EditorCommand::None;

    if (vk == kVkReturn) {
        mFocusedField = TextFieldId::None;
        mSelectedAll  = false;
        return EditorCommand::None;
    }

    std::string val = getFieldValue(mFocusedField);
    mCursorByte     = std::min(mCursorByte, val.size());

    if (ctrl && (vk == 'A' || vk == 'a')) {
        mSelectedAll = true;
        mCursorByte  = val.size();
        return EditorCommand::None;
    }
    if (ctrl && (vk == 'C' || vk == 'c')) {
        if (outCopyUtf8) *outCopyUtf8 = val;
        return EditorCommand::None;
    }
    if (ctrl && (vk == 'V' || vk == 'v')) {
        if (!clipboardUtf8.empty()) {
            insertText(clipboardUtf8);
        }
        return EditorCommand::None;
    }

    if (vk == kVkLeft) {
        mSelectedAll = false;
        mCursorByte  = prevUtf8Boundary(val, mCursorByte);
        return EditorCommand::None;
    }
    if (vk == kVkRight) {
        mSelectedAll = false;
        mCursorByte  = nextUtf8Boundary(val, mCursorByte);
        return EditorCommand::None;
    }
    if (vk == kVkHome) {
        mSelectedAll = false;
        mCursorByte  = 0;
        return EditorCommand::None;
    }
    if (vk == kVkEnd) {
        mSelectedAll = false;
        mCursorByte  = val.size();
        return EditorCommand::None;
    }

    if (vk == kVkBack) {
        if (mSelectedAll) {
            pushUndoSnapshot();
            setFieldValue(mFocusedField, "");
            mCursorByte  = 0;
            mSelectedAll = false;
            mDirty       = true;
        } else if (mCursorByte > 0) {
            pushUndoSnapshot();
            size_t prev = prevUtf8Boundary(val, mCursorByte);
            val.erase(prev, mCursorByte - prev);
            setFieldValue(mFocusedField, val);
            mCursorByte = prev;
            mDirty      = true;
        }
        return EditorCommand::None;
    }

    if (vk == kVkDelete) {
        if (mSelectedAll) {
            pushUndoSnapshot();
            setFieldValue(mFocusedField, "");
            mCursorByte  = 0;
            mSelectedAll = false;
            mDirty       = true;
        } else if (mCursorByte < val.size()) {
            pushUndoSnapshot();
            size_t next = nextUtf8Boundary(val, mCursorByte);
            val.erase(mCursorByte, next - mCursorByte);
            setFieldValue(mFocusedField, val);
            mDirty = true;
        }
        return EditorCommand::None;
    }

    if (!typedUtf8.empty()) {
        insertText(typedUtf8);
    } else if (!ctrl) {
        char ch = vkToFallbackAscii(vk, shift);
        if (ch != 0) {
            char buf[2]{ch, '\0'};
            insertText(std::string_view(buf, 1));
        }
    }
    return EditorCommand::None;
}

bool EditorModel::insertText(std::string_view utf8) {
    if (mFocusedField == TextFieldId::None) return false;

    std::string clean;
    clean.reserve(utf8.size());
    for (unsigned char c : utf8) {
        if (c == '\r' || c == '\n' || c == '\t' || c < 0x20u || c == 0x7Fu) continue;
        if (mFocusedField == TextFieldId::RulePort && !std::isdigit(c)) continue;
        clean.push_back(static_cast<char>(c));
    }
    if (clean.empty()) return false;

    std::string cur = getFieldValue(mFocusedField);
    if (mSelectedAll) {
        cur.clear();
        mCursorByte  = 0;
        mSelectedAll = false;
    }
    mCursorByte = std::min(mCursorByte, cur.size());

    size_t maxBytes = (mFocusedField == TextFieldId::SlotValue || mFocusedField == TextFieldId::MacroStepValue) ? 256
                    : (mFocusedField == TextFieldId::RulePort) ? 5
                                                               : 48;
    if (cur.size() >= maxBytes) return false;

    size_t      room    = maxBytes - cur.size();
    std::string safeAdd = truncateUtf8Bytes(clean, room);
    if (safeAdd.empty()) return false;

    pushUndoSnapshot();
    cur.insert(mCursorByte, safeAdd);
    mCursorByte += safeAdd.size();
    setFieldValue(mFocusedField, cur);
    mDirty = true;
    return true;
}

std::string* EditorModel::focusedStringPtr() noexcept {
    switch (mFocusedField) {
    case TextFieldId::PresetName:
        if (auto* p = currentPreset()) return &p->name;
        return nullptr;
    case TextFieldId::HostPattern:
        if (auto* p = currentPreset()) {
            ensureSelectedRule(*p);
            return &p->serverRules[static_cast<size_t>(mSelectedRule)].hostPattern;
        }
        return nullptr;
    case TextFieldId::ServerNamePattern:
        if (auto* p = currentPreset()) {
            ensureSelectedRule(*p);
            return &p->serverRules[static_cast<size_t>(mSelectedRule)].serverNamePattern;
        }
        return nullptr;
    case TextFieldId::WorldNamePattern:
        if (auto* p = currentPreset()) {
            ensureSelectedRule(*p);
            return &p->serverRules[static_cast<size_t>(mSelectedRule)].worldNamePattern;
        }
        return nullptr;
    case TextFieldId::RulePort:
        return &mPortEditBuf;
    case TextFieldId::SlotLabel:
        if (auto* s = currentSlot()) return &s->label;
        return nullptr;
    case TextFieldId::SlotSubtitle:
        if (auto* s = currentSlot()) return &s->subtitle;
        return nullptr;
    case TextFieldId::SlotValue:
        if (auto* s = currentSlot()) return &s->value;
        return nullptr;
    case TextFieldId::MacroStepValue:
        if (auto* s = currentSlot(); s && s->type == ActionType::Macro && mSelectedMacroStep >= 0
            && mSelectedMacroStep < static_cast<int>(s->steps.size())) {
            return &s->steps[static_cast<size_t>(mSelectedMacroStep)].value;
        }
        return nullptr;
    case TextFieldId::None:
    default:
        return nullptr;
    }
}

std::string EditorModel::getFieldValue(TextFieldId id) const {
    switch (id) {
    case TextFieldId::PresetName:
        return currentPreset() ? currentPreset()->name : "";
    case TextFieldId::HostPattern:
        return currentRule() ? currentRule()->hostPattern : "";
    case TextFieldId::ServerNamePattern:
        return currentRule() ? currentRule()->serverNamePattern : "";
    case TextFieldId::WorldNamePattern:
        return currentRule() ? currentRule()->worldNamePattern : "";
    case TextFieldId::RulePort:
        return mPortEditBuf;
    case TextFieldId::SlotLabel:
        return currentSlot() ? currentSlot()->label : "";
    case TextFieldId::SlotSubtitle:
        return currentSlot() ? currentSlot()->subtitle : "";
    case TextFieldId::SlotValue:
        return currentSlot() ? currentSlot()->value : "";
    case TextFieldId::MacroStepValue:
        if (auto const* s = currentSlot(); s && s->type == ActionType::Macro && mSelectedMacroStep >= 0
            && mSelectedMacroStep < static_cast<int>(s->steps.size())) {
            return s->steps[static_cast<size_t>(mSelectedMacroStep)].value;
        }
        return "";
    case TextFieldId::None:
    default:
        return "";
    }
}

void EditorModel::setFieldValue(TextFieldId id, std::string const& val) {
    TextFieldId prev = mFocusedField;
    mFocusedField    = id;
    if (auto* ptr = focusedStringPtr()) {
        *ptr = val;
    }
    if (id == TextFieldId::RulePort) {
        if (auto* p = currentPreset()) {
            ensureSelectedRule(*p);
            int port = val.empty() ? 0 : std::clamp(std::atoi(val.c_str()), 0, 65535);
            p->serverRules[static_cast<size_t>(mSelectedRule)].port = port;
        }
    }
    mFocusedField = prev;
}

size_t EditorModel::prevUtf8Boundary(std::string const& s, size_t pos) noexcept {
    if (pos == 0 || s.empty()) return 0;
    size_t i = std::min(pos, s.size()) - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) --i;
    return i;
}

size_t EditorModel::nextUtf8Boundary(std::string const& s, size_t pos) noexcept {
    if (pos >= s.size()) return s.size();
    size_t i = pos + 1;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) ++i;
    return i;
}

} // namespace radial::editor
