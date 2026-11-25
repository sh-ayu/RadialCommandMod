#include "mod/RadialEditorUi.h"

#include "core/RadialGeometry.h"
#include "mod/RadialRenderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ll/api/event/input/KeyInputEvent.h>
#include <ll/api/event/input/MouseInputEvent.h>
#include <ll/api/memory/Hook.h>

#include <mc/client/game/ClientInstance.h>
#include <mc/client/gui/CaretMeasureData.h>
#include <mc/client/gui/Font.h>
#include <mc/client/gui/FontHandle.h>
#include <mc/client/gui/TextAlignment.h>
#include <mc/client/gui/TextMeasureData.h>
#include <mc/client/renderer/screen/MinecraftUIRenderContext.h>
#include <mc/deps/core/math/Color.h>
#include <mc/deps/input/MouseAction.h>
#include <mc/deps/input/RectangleArea.h>

namespace radial {
namespace {

using Clock = std::chrono::steady_clock;

int64_t toNs(Clock::time_point t) noexcept { return t.time_since_epoch().count(); }

std::atomic<int64_t> gGrabBlockedUntilNs{0};

LL_TYPE_INSTANCE_HOOK(
    RadialEditorGrabMouseHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$grabMouse,
    void
) {
    if (toNs(Clock::now()) < gGrabBlockedUntilNs.load(std::memory_order_acquire)) {
        return;
    }
    origin();
}

// Translate a virtual key using the active OS keyboard layout (fixes JIS 106/109 symbols like :, @, _, *, /).
std::string translateVkToUtf8(int vk, bool ctrl, bool alt) noexcept {
    // Skip pure Ctrl shortcuts (Ctrl+A/C/V/S/Z), but allow AltGr (Ctrl+Alt).
    if (ctrl && !alt) return {};
    if (vk == VK_BACK || vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_TAB || vk == VK_DELETE) return {};
    try {
        BYTE kbState[256]{};
        if (!GetKeyboardState(kbState)) return {};
        // Refresh async modifier states so Shift/CapsLock/AltGr are always accurate on HID thread.
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) kbState[VK_SHIFT] |= 0x80;
        if (GetAsyncKeyState(VK_LSHIFT) & 0x8000) kbState[VK_LSHIFT] |= 0x80;
        if (GetAsyncKeyState(VK_RSHIFT) & 0x8000) kbState[VK_RSHIFT] |= 0x80;
        if (GetKeyState(VK_CAPITAL) & 0x0001) kbState[VK_CAPITAL] |= 0x01;
        else kbState[VK_CAPITAL] &= ~0x01;
        if (!ctrl) {
            kbState[VK_CONTROL]  = 0;
            kbState[VK_LCONTROL] = 0;
            kbState[VK_RCONTROL] = 0;
        }

        HKL     layout   = GetKeyboardLayout(0);
        UINT    scanCode = MapVirtualKeyExW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC, layout);
        wchar_t wbuf[8]{};
        // Flag 0x4 = do not disturb dead-key keyboard buffer state.
        int n = ToUnicodeEx(static_cast<UINT>(vk), scanCode, kbState, wbuf, 8, 0x4u, layout);
        if (n <= 0) return {};

        int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf, n, nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wbuf, n, out.data(), needed, nullptr, nullptr);
        return out;
    } catch (...) {
        return {};
    }
}

std::string readClipboardUtf8() noexcept {
    try {
        if (!OpenClipboard(nullptr)) return {};
        std::string out;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto const* ws = static_cast<wchar_t const*>(GlobalLock(h))) {
                int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 1) {
                    out.resize(static_cast<size_t>(needed - 1));
                    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), needed, nullptr, nullptr);
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return out;
    } catch (...) {
        return {};
    }
}

bool writeClipboardUtf8(std::string_view utf8) noexcept {
    try {
        int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed < 0) return false;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(needed + 1) * sizeof(wchar_t));
        if (!hMem) return false;
        auto* dst = static_cast<wchar_t*>(GlobalLock(hMem));
        if (!dst) {
            GlobalFree(hMem);
            return false;
        }
        if (needed > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), dst, needed);
        dst[needed] = L'\0';
        GlobalUnlock(hMem);
        if (!OpenClipboard(nullptr)) {
            GlobalFree(hMem);
            return false;
        }
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
            CloseClipboard();
            GlobalFree(hMem);
            return false;
        }
        CloseClipboard();
        return true;
    } catch (...) {
        return false;
    }
}

bool getGuiCursorPos(float screenW, float screenH, float& outX, float& outY) noexcept {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return false;
    POINT pt{};
    RECT  rc{};
    if (!GetCursorPos(&pt) || !ScreenToClient(fg, &pt) || !GetClientRect(fg, &rc)) return false;
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    outX = static_cast<float>(pt.x) * screenW / static_cast<float>(rc.right);
    outY = static_cast<float>(pt.y) * screenH / static_cast<float>(rc.bottom);
    return true;
}

RectangleArea toArea(geom::RectF const& r, float ox, float oy) noexcept {
    RectangleArea a;
    a._x0 = r.x0 + ox;
    a._y0 = r.y0 + oy;
    a._x1 = r.x1 + ox;
    a._y1 = r.y1 + oy;
    return a;
}

mce::Color fromRgb(uint32_t rgb) noexcept {
    return mce::Color(
        static_cast<int>((rgb >> 16) & 0xFFu),
        static_cast<int>((rgb >> 8) & 0xFFu),
        static_cast<int>(rgb & 0xFFu)
    );
}

mce::Color blendColor(mce::Color const& a, mce::Color const& b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return mce::Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f);
}

mce::Color slotColor(RadialSlot const& s) noexcept {
    if (auto rgb = parseHexColor(s.color)) return fromRgb(*rgb);
    switch (s.type) {
    case ActionType::Command:       return mce::Color(56, 189, 248);
    case ActionType::Chat:          return mce::Color(52, 211, 153);
    case ActionType::ClientCommand: return mce::Color(245, 158, 11);
    case ActionType::Macro:         return mce::Color(167, 139, 250);
    case ActionType::Submenu:       return mce::Color(96, 165, 250);
    }
    return mce::Color(56, 189, 248);
}

Font* getFont(MinecraftUIRenderContext& ctx) noexcept {
    try {
        return ctx.mDebugTextFontHandle.get().mDefaultFont.get();
    } catch (...) {
        return nullptr;
    }
}

TextWidthMeasureFn makeMeasure(Font* font, float scale) noexcept {
    return [font, scale](std::string_view sv) -> float {
        if (sv.empty()) return 0.0f;
        if (font) {
            try {
                // Measure at 1.0f then scale in float to avoid integer rounding drift at fractional scales (e.g. 0.68f).
                int px = font->getLineLength(sv, 1.0f, false);
                if (px > 0) return static_cast<float>(px) * scale;
            } catch (...) {
            }
        }
        return estimateDisplayWidth(sv, scale);
    };
}

void drawUiText(
    MinecraftUIRenderContext& ctx,
    Font*                     font,
    geom::RectF const&        r,
    float                     ox,
    float                     oy,
    std::string               text,
    mce::Color const&         col,
    float                     alpha,
    float                     scale = 0.72f,
    ::ui::TextAlignment       align = ::ui::TextAlignment::Left,
    bool                      fitToRect = true
) noexcept {
    if (text.empty() || alpha <= 0.01f) return;
    try {
        std::string fitted = fitToRect
            ? fitTextToWidth(text, std::max(4.0f, r.width()), scale, makeMeasure(font, scale))
            : std::move(text);
        if (fitted.empty()) return;
        TextMeasureData  m{scale, 0.0f, true, false, false, align};
        CaretMeasureData c{0, false};
        auto             a = toArea(r, ox, oy);
        if (font) ctx.drawText(*font, a, std::move(fitted), col, alpha, align, m, c);
        else ctx.drawDebugText(a, std::move(fitted), col, alpha, align, m, c);
    } catch (...) {
    }
}

void fillRects(
    MinecraftUIRenderContext&       ctx,
    std::vector<geom::RectF> const& rects,
    float                           ox,
    float                           oy,
    mce::Color const&               col,
    float                           alpha
) noexcept {
    if (alpha <= 0.005f) return;
    for (auto const& r : rects) {
        ctx.fillRectangle(toArea(r, ox, oy), col, alpha);
    }
}

inline constexpr mce::Color kPanelBg(12, 16, 24);
inline constexpr mce::Color kPaneBg(17, 23, 35);
inline constexpr mce::Color kHeaderBg(22, 30, 46);
inline constexpr mce::Color kTabBarBg(15, 20, 31);
inline constexpr mce::Color kBannerBg(68, 42, 14);
inline constexpr mce::Color kBtnBg(30, 41, 59);
inline constexpr mce::Color kBtnDisabled(22, 29, 43);
inline constexpr mce::Color kBtnHover(48, 63, 90);
inline constexpr mce::Color kBtnActive(37, 99, 235);
inline constexpr mce::Color kBtnAccent(14, 116, 144);
inline constexpr mce::Color kBtnSuccess(22, 163, 74);
inline constexpr mce::Color kBtnDanger(153, 27, 27);
inline constexpr mce::Color kBorder(71, 85, 105);
inline constexpr mce::Color kBorderFocus(56, 189, 248);
inline constexpr mce::Color kText(241, 245, 249);
inline constexpr mce::Color kMuted(148, 163, 184);
inline constexpr mce::Color kGreen(52, 211, 153);

void renderWheelPreview(
    MinecraftUIRenderContext&         ctx,
    Font*                             font,
    editor::WheelPreviewLayout const& wp,
    ResolvedLang                      lang,
    float                             ox,
    float                             oy,
    float                             mouseX,
    float                             mouseY
) noexcept {
    if (!wp.visible || wp.slots.empty()) return;

    int const   hoverHit = wp.bounds.contains(mouseX, mouseY)
                             ? editor::EditorModel::hitTestWheelPreview(wp, mouseX, mouseY)
                             : -1;
    int const   count    = static_cast<int>(wp.slots.size());
    float const hubR     = wp.innerR;

    thread_local WheelMeshBatch mesh;
    mesh.clear();
    mesh.reserve(2000);

    // 1) Seamless backdrop disc behind the preview wheel.
    mesh.addDisc(wp.cx, wp.cy, wp.outerR + 1.5f, mce::Color(6, 9, 15), 0.52f);

    constexpr float kTwoPi  = 6.28318530718f;
    float const     midR    = wp.innerR + (wp.outerR - wp.innerR) * 0.54f;
    float const     arcW    = (kTwoPi * midR / static_cast<float>(count)) * 0.85f;
    float const     lblW    = std::clamp(std::min(arcW, (wp.outerR - wp.innerR) * 1.4f), 22.0f, 64.0f);
    float const     lblSc   = std::clamp(wp.fontScale * (count > 8 ? 0.58f : 0.68f), 0.48f, 0.85f);
    float const     halfGap = 0.55f;

    for (int i = 0; i < count; ++i) {
        auto const& slot   = wp.slots[static_cast<size_t>(i)];
        bool const  isSel  = (i == wp.selected);
        bool const  isHov  = (i == hoverHit);
        auto const  ang    = geom::computeSectorAngles(i, count, 0.0f);
        mce::Color  accent = slotColor(slot);

        float const rIn    = wp.innerR;
        float const rOut   = isSel ? (wp.outerR + 3.5f) : (isHov ? (wp.outerR + 1.8f) : wp.outerR);
        float const rimTh  = isSel ? 2.8f : 1.8f;
        float const rRimIn = std::max(rIn + 2.0f, rOut - rimTh);

        mce::Color fillCol = isSel ? blendColor(kPanelBg, accent, 0.36f)
                           : isHov ? blendColor(kPanelBg, accent, 0.22f)
                                   : kPanelBg;
        float fillA = std::clamp(wp.bgOpacity + (isSel ? 0.14f : (isHov ? 0.08f : 0.0f)), 0.20f, 0.96f);

        mesh.addAnnularSector(wp.cx, wp.cy, rIn, rRimIn, ang.startAngle, ang.endAngle, halfGap, fillCol, fillA);
        mesh.addAnnularSector(wp.cx, wp.cy, rRimIn, rOut, ang.startAngle, ang.endAngle, halfGap, isSel ? kText : accent, isSel ? 0.96f : (isHov ? 0.85f : 0.60f));
    }

    // Center hub flush with innerR (zero gap!).
    mesh.addDisc(wp.cx, wp.cy, hubR, (hoverHit == -2 && wp.inSubmenu) ? kBtnHover : kPanelBg, 0.96f);
    mesh.addRing(wp.cx, wp.cy, std::max(1.0f, hubR - 1.2f), hubR + 0.2f, (hoverHit == -2 && wp.inSubmenu) ? kBorderFocus : kBorder, 0.82f);

    if (std::abs(wp.deadzoneR - hubR) > 5.0f) {
        mesh.addRing(wp.cx, wp.cy, std::max(1.0f, wp.deadzoneR - 0.7f), wp.deadzoneR, kMuted, 0.28f);
    }

    if (!mesh.flush(ctx, ox, oy)) {
        std::vector<geom::RectF> buf;
        buf.reserve(160);
        for (int i = 0; i < count; ++i) {
            auto const& slot   = wp.slots[static_cast<size_t>(i)];
            bool const  isSel  = (i == wp.selected);
            auto const  ang    = geom::computeSectorAngles(i, count, 0.02f);
            mce::Color  accent = slotColor(slot);
            buf.clear();
            geom::rasterizeAnnularSector(wp.cx, wp.cy, wp.innerR, isSel ? (wp.outerR + 3.5f) : wp.outerR, ang.startAngle, ang.endAngle, 2.0f, buf);
            fillRects(ctx, buf, ox, oy, isSel ? blendColor(kPanelBg, accent, 0.36f) : kPanelBg, 0.86f);
        }
        buf.clear();
        geom::rasterizeDisc(wp.cx, wp.cy, hubR, 2.0f, buf);
        fillRects(ctx, buf, ox, oy, kPanelBg, 0.96f);
    }

    for (int i = 0; i < count; ++i) {
        auto const& slot  = wp.slots[static_cast<size_t>(i)];
        bool const  isSel = (i == wp.selected);
        bool const  isHov = (i == hoverHit);
        float const rOut  = isSel ? (wp.outerR + 3.5f) : (isHov ? (wp.outerR + 1.8f) : wp.outerR);
        auto        pt    = geom::sectorLabelCenter(wp.cx, wp.cy, wp.innerR + (rOut - wp.innerR) * 0.54f, i, count);
        std::string lbl   = slot.label;
        if (slot.type == ActionType::Submenu) lbl += ">";
        drawUiText(
            ctx, font,
            {pt.x - lblW * 0.5f, pt.y - 4.5f, pt.x + lblW * 0.5f, pt.y + 5.5f},
            ox, oy, lbl, (isSel || isHov) ? kText : mce::Color(203, 213, 225), 1.0f, lblSc, ::ui::TextAlignment::Center
        );
    }

    std::string hubTxt = wp.inSubmenu ? std::string(tr(lang, "hud.back")) : (std::to_string(count) + " " + tr(lang, "ed.wheel.divisions"));
    drawUiText(
        ctx, font,
        {wp.cx - hubR + 2.0f, wp.cy - 4.0f, wp.cx + hubR - 2.0f, wp.cy + 5.0f},
        ox, oy, hubTxt, wp.inSubmenu ? kBorderFocus : kMuted, 0.95f, 0.58f, ::ui::TextAlignment::Center
    );
}

} // namespace

RadialEditorUi::RadialEditorUi() = default;

bool RadialEditorUi::installHooks() noexcept {
    if (mHookInstalled) return true;
    try {
        mHookInstalled = (RadialEditorGrabMouseHook::hook() == 0);
        return mHookInstalled;
    } catch (...) {
        return false;
    }
}

void RadialEditorUi::uninstallHooks() noexcept {
    mActive.store(false, std::memory_order_release);
    gGrabBlockedUntilNs.store(0, std::memory_order_release);
    if (mHookInstalled) {
        try {
            RadialEditorGrabMouseHook::unhook();
        } catch (...) {
        }
        mHookInstalled = false;
    }
}

void RadialEditorUi::clearCallbacks() {
    std::lock_guard lock(mMutex);
    mOnSave   = {};
    mOnReload = {};
    mOnTest   = {};
}

void RadialEditorUi::setCallbacks(SaveCallback onSave, ReloadCallback onReload, TestCallback onTest) {
    std::lock_guard lock(mMutex);
    mOnSave   = std::move(onSave);
    mOnReload = std::move(onReload);
    mOnTest   = std::move(onTest);
}

void RadialEditorUi::requestOpen(
    RadialConfig const&   cfg,
    ServerIdentity const& server,
    int                   activePresetIdx,
    bool                  systemIsJapanese
) {
    {
        std::lock_guard lock(mMutex);
        mPendingConfig     = cfg;
        mPendingServer     = server;
        mPendingPresetIdx  = activePresetIdx;
        mPendingSystemIsJa = systemIsJapanese;
        mKeyQueue.clear();
    }
    mLeftClickPending.store(false, std::memory_order_relaxed);
    mLeftButtonHeld.store(false, std::memory_order_relaxed);
    mWheelDelta.store(0, std::memory_order_relaxed);
    mCapturedMouseBtn.store(0, std::memory_order_relaxed);
    mClosePending.store(false, std::memory_order_relaxed);
    mOpenRequested.store(true, std::memory_order_release);
}

void RadialEditorUi::requestClose(bool regrabMouse) noexcept {
    if (!mActive.load(std::memory_order_acquire) && !mOpenRequested.load(std::memory_order_acquire)) return;
    mOpenRequested.store(false, std::memory_order_release);
    mActive.store(false, std::memory_order_release);
    mSuspended.store(false, std::memory_order_release);
    mLeftButtonHeld.store(false, std::memory_order_relaxed);
    gGrabBlockedUntilNs.store(0, std::memory_order_release);
    mRegrabOnClose.store(regrabMouse, std::memory_order_relaxed);
    mJustClosed.store(true, std::memory_order_release);
    mClosePending.store(true, std::memory_order_release);
}

void RadialEditorUi::setSuspended(bool suspended) noexcept {
    if (!mActive.load(std::memory_order_acquire)) {
        mSuspended.store(false, std::memory_order_release);
        return;
    }
    bool prev = mSuspended.exchange(suspended, std::memory_order_acq_rel);
    if (suspended && !prev) {
        gGrabBlockedUntilNs.store(0, std::memory_order_release);
        mLeftButtonHeld.store(false, std::memory_order_relaxed);
        mLeftClickPending.store(false, std::memory_order_relaxed);
        mWheelDelta.store(0, std::memory_order_relaxed);
        mCapturedMouseBtn.store(0, std::memory_order_relaxed);
        std::lock_guard lock(mMutex);
        mKeyQueue.clear();
    }
}

void RadialEditorUi::forceCloseOnDisable(ClientInstance* client) noexcept {
    bool wasOpen      = mActive.exchange(false, std::memory_order_acq_rel);
    bool weReleased   = mWeReleasedMouse.exchange(false, std::memory_order_acq_rel);
    mSuspended.store(false, std::memory_order_release);
    mOpenRequested.store(false, std::memory_order_release);
    mClosePending.store(false, std::memory_order_release);
    gGrabBlockedUntilNs.store(0, std::memory_order_release);
    if ((wasOpen || weReleased) && client) {
        try {
            if (!client->isShowingMenu() && client->isInWorldAndNotShowingAnyMenuScreens()) {
                client->grabMouse();
            }
        } catch (...) {
        }
    }
}

bool RadialEditorUi::onKeyInput(ll::event::input::KeyInputEvent& ev) noexcept {
    try {
        if (!mActive.load(std::memory_order_acquire)) return false;
        if (mSuspended.load(std::memory_order_acquire)) return false;
        // Never close or steal keys while another window is focused (e.g. Alt-Tab to browser/Discord).
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) return false;

        // Always let KeyUp pass through to the game so movement/sprint keys never stick!
        if (!ev.isDown()) return true;

        ev.cancel();

        int  vk    = ev.keyCode();
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        std::string typed = translateVkToUtf8(vk, ctrl, alt);

        std::lock_guard lock(mMutex);
        if (mKeyQueue.size() < 64) {
            mKeyQueue.push_back(PendingKey{vk, shift, ctrl, std::move(typed)});
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool RadialEditorUi::onMouseInput(ll::event::input::MouseInputEvent& ev) noexcept {
    try {
        if (!mActive.load(std::memory_order_acquire)) return false;
        if (mSuspended.load(std::memory_order_acquire)) return false;
        // Never close or steal mouse events while another window is focused.
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) return false;

        char  btn  = ev.actionButtonId();
        schar data = ev.buttonData();

        if (btn == MouseAction::ActionWheel) {
            if (data != 0) mWheelDelta.fetch_add(data > 0 ? 1 : -1, std::memory_order_relaxed);
            ev.cancel();
            return true;
        }

        if (btn == MouseAction::ActionLeft) {
            if (data == MouseAction::DataDown) {
                mLeftClickPending.store(true, std::memory_order_relaxed);
                mLeftButtonHeld.store(true, std::memory_order_relaxed);
                ev.cancel();
                return true;
            }
            if (data == MouseAction::DataUp) {
                mLeftButtonHeld.store(false, std::memory_order_relaxed);
                return true; // Let button release reach the game so held attacks/mining release cleanly.
            }
        }

        if (data == MouseAction::DataDown) {
            int mv = (btn == MouseAction::ActionMiddle) ? kVkMButton
                   : (btn == MouseAction::ActionX1)     ? kVkXButton1
                   : (btn == MouseAction::ActionX2)     ? kVkXButton2 : 0;
            if (mv != 0) mCapturedMouseBtn.store(mv, std::memory_order_relaxed);
        }

        bool isButton = btn == MouseAction::ActionLeft || btn == MouseAction::ActionRight
                     || btn == MouseAction::ActionMiddle || btn == MouseAction::ActionX1
                     || btn == MouseAction::ActionX2;
        if (isButton && data == MouseAction::DataUp) return true;
        ev.cancel();
        return true;
    } catch (...) {
        return false;
    }
}

void RadialEditorUi::keepCursorVisible(ClientInstance& client, Clock::time_point now) noexcept {
    try {
        CURSORINFO info{};
        info.cbSize = sizeof(info);
        bool hidden = GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING);
        if (!client.getMouseGrabbed() && !hidden) return;
        if (now - mLastRelease < std::chrono::milliseconds(200)) return;
        mLastRelease = now;
        client.releaseMouse();
    } catch (...) {
    }
}

void RadialEditorUi::onHudRender(
    ClientInstance&           client,
    MinecraftUIRenderContext& ctx,
    float                     originX,
    float                     originY,
    float                     screenW,
    float                     screenH,
    bool                      inWorld,
    bool                      safeHud,
    bool                      isForeground
) noexcept {
    try {
        auto now = Clock::now();

        if (mClosePending.exchange(false, std::memory_order_acq_rel)) {
            gGrabBlockedUntilNs.store(0, std::memory_order_release);
            bool weRel = mWeReleasedMouse.exchange(false, std::memory_order_acq_rel);
            if (weRel && mRegrabOnClose.load(std::memory_order_relaxed) && safeHud && isForeground) {
                client.grabMouse();
            }
        }

        if (mOpenRequested.exchange(false, std::memory_order_acq_rel)) {
            if (!inWorld || !safeHud) return;
            {
                std::lock_guard lock(mMutex);
                mModel.beginSession(mPendingConfig, mPendingServer, mPendingPresetIdx, mPendingSystemIsJa);
                mKeyQueue.clear();
            }
            mAltWasDownForCapture.store((GetAsyncKeyState(VK_MENU) & 0x8000) != 0, std::memory_order_relaxed);
            mLastFrameNs.store(toNs(now), std::memory_order_release);
            gGrabBlockedUntilNs.store(toNs(now + std::chrono::milliseconds(160)), std::memory_order_release);
            mSuspended.store(false, std::memory_order_release);
            mActive.store(true, std::memory_order_release);
            mWeReleasedMouse.store(true, std::memory_order_release);
            mWasForeground     = isForeground;
            mLastRelease       = now;
            mCaretBlinkOrigin  = now;
            mStatusPinnedUntil = Clock::time_point{};
            mLastFocusedField  = editor::TextFieldId::None;
            client.releaseMouse();
        }

        if (!mActive.load(std::memory_order_acquire)) return;

        // Only close automatically if actually leaving the world/server.
        if (!inWorld) {
            requestClose(false);
            return;
        }

        // Temporarily suspended (e.g. singleplayer pause screen opened on Alt-Tab): keep draft alive!
        if (!safeHud) {
            setSuspended(true);
            return;
        }

        // Resuming from pause screen back into HUD!
        bool wasSuspended = mSuspended.exchange(false, std::memory_order_acq_rel);
        if (wasSuspended) {
            mCaretBlinkOrigin = now;
            mLastRelease      = Clock::time_point{};
            if (isForeground) {
                gGrabBlockedUntilNs.store(toNs(now + std::chrono::milliseconds(160)), std::memory_order_release);
                client.releaseMouse();
            }
        }

        mLastFrameNs.store(toNs(now), std::memory_order_release);

        // When another window is focused (e.g. browser/Discord in multiplayer/windowed), keep the editor
        // visible & intact without closing, stealing cursor, or running clicks/keys.
        if (!isForeground) {
            mWasForeground = false;
            mLeftButtonHeld.store(false, std::memory_order_relaxed);
            mLeftClickPending.store(false, std::memory_order_relaxed);
            mWheelDelta.store(0, std::memory_order_relaxed);
            gGrabBlockedUntilNs.store(0, std::memory_order_release);
            {
                std::lock_guard lock(mMutex);
                mKeyQueue.clear();
            }
            renderPanel(ctx, originX, originY, screenW, screenH, -1000.0f, -1000.0f, false, now);
            return;
        }

        if (!mWasForeground) {
            mWasForeground    = true;
            mCaretBlinkOrigin = now;
            mLastRelease      = Clock::time_point{};
        }

        gGrabBlockedUntilNs.store(toNs(now + std::chrono::milliseconds(160)), std::memory_order_release);
        keepCursorVisible(client, now);

        float mx = -1000.0f;
        float my = -1000.0f;
        getGuiCursorPos(screenW, screenH, mx, my);

        // Drain key queue first and pre-read clipboard OUTSIDE mMutex if Ctrl+V is queued.
        std::vector<PendingKey> keys;
        {
            std::lock_guard lock(mMutex);
            keys.swap(mKeyQueue);
        }

        bool needPaste = false;
        for (auto const& k : keys) {
            if (k.ctrl && (k.vk == 'V' || k.vk == 'v')) {
                needPaste = true;
                break;
            }
        }
        std::string clipboardText = needPaste ? readClipboardUtf8() : std::string{};

        // Also detect Alt key via GetAsyncKeyState while capturing a keybind (since WM_SYSKEYDOWN skips KeyInputEvent).
        bool altNow   = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
                     || ((GetAsyncKeyState(VK_LMENU) & 0x8000) != 0)
                     || ((GetAsyncKeyState(VK_RMENU) & 0x8000) != 0);
        bool altPrev  = mAltWasDownForCapture.exchange(altNow, std::memory_order_relaxed);
        bool altEdge  = altNow && !altPrev;

        bool physLeft = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
        if (!physLeft) mLeftButtonHeld.store(false, std::memory_order_relaxed);

        Font* inputFont    = getFont(ctx);
        auto  inputMeasure = makeMeasure(inputFont, 0.68f);

        editor::EditorCommand pendingCmd = editor::EditorCommand::None;
        SaveCallback          saveCb;
        ReloadCallback        reloadCb;
        TestCallback          testCb;
        RadialConfig          draftCopy;
        RadialSlot            testSlotCopy;
        std::string           copyToClipboard;

        auto mergeCmd = [&](editor::EditorCommand c) {
            if (c == editor::EditorCommand::None) return;
            if (pendingCmd == editor::EditorCommand::Save && c == editor::EditorCommand::Close) {
                pendingCmd = editor::EditorCommand::SaveAndClose;
            } else if (pendingCmd != editor::EditorCommand::SaveAndClose) {
                pendingCmd = c;
            }
        };

        {
            std::lock_guard lock(mMutex);

            if (altEdge && mModel.captureTarget() != editor::CaptureTarget::None) {
                mModel.handleCapturedKey(kVkMenu);
            }

            int capMouse = mCapturedMouseBtn.exchange(0, std::memory_order_relaxed);
            if (capMouse != 0) {
                mModel.handleMouseButtonForCapture(capMouse);
            }

            int wh = mWheelDelta.exchange(0, std::memory_order_relaxed);
            if (wh != 0) {
                mModel.handleMouseWheel(wh);
            }

            if (mLeftClickPending.exchange(false, std::memory_order_relaxed)) {
                mCaretBlinkOrigin = now;
                auto dtMs         = std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastClickTime).count();
                bool isDbl        = (dtMs >= 0 && dtMs <= 350)
                                 && (std::abs(mx - mLastClickX) <= 4.0f && std::abs(my - mLastClickY) <= 4.0f);
                mLastClickTime    = now;
                mLastClickX       = mx;
                mLastClickY       = my;
                std::string prevSt = mModel.statusMessage();
                mergeCmd(mModel.handleClick(mx, my, screenW, screenH, inputMeasure, isDbl));
                if (mModel.statusMessage() != prevSt) {
                    mStatusPinnedUntil = now + std::chrono::milliseconds(2200);
                }
            } else if (mLeftButtonHeld.load(std::memory_order_relaxed) && mModel.isDraggingSlider()) {
                mModel.handleDrag(mx, my, screenW, screenH);
            } else if (!mLeftButtonHeld.load(std::memory_order_relaxed) && mModel.isDraggingSlider()) {
                mModel.endDrag();
            }

            if (!keys.empty()) {
                mCaretBlinkOrigin = now;
            }
            for (auto const& k : keys) {
                std::string copied;
                std::string prevSt = mModel.statusMessage();
                mergeCmd(mModel.handleKey(
                    k.vk,
                    k.shift,
                    k.ctrl,
                    (k.ctrl && (k.vk == 'V' || k.vk == 'v')) ? std::string_view(clipboardText) : std::string_view{},
                    k.typedUtf8,
                    &copied
                ));
                if (mModel.statusMessage() != prevSt) {
                    mStatusPinnedUntil = now + std::chrono::milliseconds(2200);
                }
                if (!copied.empty()) copyToClipboard = std::move(copied);
            }

            if (pendingCmd == editor::EditorCommand::Save || pendingCmd == editor::EditorCommand::SaveAndClose) {
                saveCb    = mOnSave;
                draftCopy = mModel.draft();
            } else if (pendingCmd == editor::EditorCommand::Reload) {
                reloadCb = mOnReload;
            } else if (pendingCmd == editor::EditorCommand::TestSlot) {
                if (auto const* s = mModel.currentSlot()) {
                    testCb       = mOnTest;
                    testSlotCopy = *s;
                }
            }
        }

        if (!copyToClipboard.empty()) {
            writeClipboardUtf8(copyToClipboard);
        }

        if ((pendingCmd == editor::EditorCommand::Save || pendingCmd == editor::EditorCommand::SaveAndClose) && saveCb) {
            std::string msg;
            bool        ok = saveCb(draftCopy, msg);
            {
                std::lock_guard lock(mMutex);
                if (ok) mModel.markSaved();
                ResolvedLang l = mModel.resolvedLang();
                mModel.setStatus(msg.empty() ? tr(l, ok ? "ed.status.saved" : "ed.status.save_fail") : msg);
            }
            mStatusPinnedUntil = now + std::chrono::milliseconds(2500);
            if (ok && pendingCmd == editor::EditorCommand::SaveAndClose) {
                requestClose(true);
                return;
            }
        } else if (pendingCmd == editor::EditorCommand::Reload && reloadCb) {
            RadialConfig reloaded;
            std::string  msg;
            if (reloadCb(reloaded, msg)) {
                std::lock_guard lock(mMutex);
                mModel.beginSession(reloaded, mModel.serverIdentity(), mModel.selectedPreset(), mPendingSystemIsJa);
                mModel.setStatus(msg.empty() ? tr(mModel.resolvedLang(), "ed.status.reloaded") : msg);
            } else {
                std::lock_guard lock(mMutex);
                mModel.setStatus(msg);
            }
            mStatusPinnedUntil = now + std::chrono::milliseconds(2500);
        } else if (pendingCmd == editor::EditorCommand::TestSlot && testCb) {
            testCb(testSlotCopy);
            mStatusPinnedUntil = now + std::chrono::milliseconds(2200);
        } else if (pendingCmd == editor::EditorCommand::Close) {
            requestClose(true);
            return;
        }

        renderPanel(ctx, originX, originY, screenW, screenH, mx, my, true, now);
    } catch (...) {
    }
}

void RadialEditorUi::renderPanel(
    MinecraftUIRenderContext& ctx,
    float                     originX,
    float                     originY,
    float                     screenW,
    float                     screenH,
    float                     mouseX,
    float                     mouseY,
    bool                      isForeground,
    Clock::time_point         now
) noexcept {
    editor::EditorLayout layout;
    std::string          status;
    std::string          serverText;
    std::string          slotTitle;

    {
        std::lock_guard lock(mMutex);
        layout     = mModel.buildLayout(screenW, screenH);
        status     = mModel.statusMessage();
        serverText = mModel.serverIdentity().displayLabel();
        if (auto const* s = mModel.currentSlot()) {
            slotTitle = std::string(tr(layout.lang, "ed.slot.header"))
                      + std::to_string(mModel.selectedSlot() + 1) + " : " + s->label;
        } else {
            slotTitle = tr(layout.lang, "ed.slot.header");
        }
    }

    Font*        font = getFont(ctx);
    ResolvedLang lang = layout.lang;

    ctx.fillRectangle(toArea({0.0f, 0.0f, screenW, screenH}, originX, originY), mce::Color(0, 0, 0), 0.48f);

    ctx.fillRectangle(toArea(layout.panel, originX, originY), kPanelBg, 0.96f);
    ctx.drawRectangle(toArea(layout.panel, originX, originY), kBorder, 0.85f, 1);
    ctx.fillRectangle(toArea(layout.header, originX, originY), kHeaderBg, 0.98f);
    ctx.fillRectangle(toArea(layout.tabBar, originX, originY), kTabBarBg, 0.96f);

    // Header text strictly bounded inside headerTextArea (never collides with header buttons!).
    drawUiText(
        ctx, font,
        {layout.headerTextArea.x0, layout.header.y0 + 2.5f, layout.headerTextArea.x1, layout.header.y0 + 12.5f},
        originX, originY, tr(lang, "ed.title"), kText, 1.0f, 0.80f
    );
    drawUiText(
        ctx, font,
        {layout.headerTextArea.x0, layout.header.y0 + 12.5f, layout.headerTextArea.x1, layout.header.y1},
        originX, originY, std::string(tr(lang, "ed.connected")) + serverText, kMuted, 0.92f, 0.62f
    );

    if (layout.confirmingClose) {
        ctx.fillRectangle(toArea(layout.confirmBanner, originX, originY), kBannerBg, 0.95f);
        ctx.drawRectangle(toArea(layout.confirmBanner, originX, originY), mce::Color(245, 158, 11), 0.85f, 1);
        drawUiText(
            ctx, font,
            {layout.confirmBanner.x0 + 5.0f, layout.confirmBanner.y0 + 3.5f, layout.confirmBanner.x1 - 180.0f, layout.confirmBanner.y1},
            originX, originY, tr(lang, "ed.unsaved.prompt"), kText, 1.0f, 0.70f
        );
    }

    for (auto const* pane : {&layout.leftPane, &layout.rightPane}) {
        ctx.fillRectangle(toArea(*pane, originX, originY), kPaneBg, 0.92f);
        ctx.drawRectangle(toArea(*pane, originX, originY), kBorder, 0.45f, 1);
    }

    if (layout.activeTab == editor::EditorTab::Wheel) {
        drawUiText(ctx, font, {layout.rightPane.x0 + 5.0f, layout.rightPane.y0 + 3.0f, layout.rightPane.x1 - 56.0f, layout.rightPane.y0 + 12.5f},
                   originX, originY, slotTitle, kBorderFocus, 1.0f, 0.72f);
        renderWheelPreview(ctx, font, layout.wheelPreview, lang, originX, originY, mouseX, mouseY);
    } else if (layout.activeTab == editor::EditorTab::Servers) {
        drawUiText(ctx, font, {layout.leftPane.x0 + 5.0f, layout.leftPane.y0 + 3.0f, layout.leftPane.x1 - 5.0f, layout.leftPane.y0 + 12.5f},
                   originX, originY, tr(lang, "ed.srv.list"), kBorderFocus, 1.0f, 0.72f);
        drawUiText(ctx, font, {layout.rightPane.x0 + 5.0f, layout.rightPane.y0 + 3.0f, layout.rightPane.x1 - 62.0f, layout.rightPane.y0 + 12.5f},
                   originX, originY, tr(lang, "ed.srv.rules"), kBorderFocus, 1.0f, 0.72f);

        geom::RectF badge{layout.rightPane.x1 - 58.0f, layout.rightPane.y0 + 2.0f, layout.rightPane.x1 - 5.0f, layout.rightPane.y0 + 12.5f};
        ctx.fillRectangle(toArea(badge, originX, originY), layout.presetMatchesCurrent ? kBtnSuccess : kBtnBg, 0.92f);
        drawUiText(ctx, font, {badge.x0, badge.y0 + 1.5f, badge.x1, badge.y1},
                   originX, originY, tr(lang, layout.presetMatchesCurrent ? "ed.srv.match" : "ed.srv.nomatch"), kText, 1.0f, 0.62f, ::ui::TextAlignment::Center);
    } else if (layout.activeTab == editor::EditorTab::Settings) {
        drawUiText(ctx, font, {layout.leftPane.x0 + 5.0f, layout.leftPane.y0 + 3.0f, layout.leftPane.x1 - 5.0f, layout.leftPane.y0 + 12.5f},
                   originX, originY, tr(lang, "ed.set.keys_hdr"), kBorderFocus, 1.0f, 0.72f);
        drawUiText(ctx, font, {layout.rightPane.x0 + 5.0f, layout.rightPane.y0 + 3.0f, layout.rightPane.x1 - 5.0f, layout.rightPane.y0 + 12.5f},
                   originX, originY, tr(lang, "ed.set.tune_hdr"), kBorderFocus, 1.0f, 0.72f);
    }

    // Sliders
    for (auto const& sl : layout.sliders) {
        bool hover = sl.rect.contains(mouseX, mouseY);
        drawUiText(ctx, font, {sl.rect.x0, sl.rect.y0 - 8.5f, sl.rect.x1 - 40.0f, sl.rect.y0},
                   originX, originY, sl.label, hover ? kText : kMuted, 0.96f, 0.64f);
        drawUiText(ctx, font, {sl.rect.x1 - 42.0f, sl.rect.y0 - 8.5f, sl.rect.x1, sl.rect.y0},
                   originX, originY, sl.valueText, kBorderFocus, 1.0f, 0.66f, ::ui::TextAlignment::Right);

        ctx.fillRectangle(toArea(sl.rect, originX, originY), kPanelBg, 0.96f);
        ctx.drawRectangle(toArea(sl.rect, originX, originY), hover ? kBorderFocus : kBorder, 0.78f, 1);

        float fillW = std::max(0.0f, (sl.rect.width() - 2.0f) * std::clamp(sl.ratio, 0.0f, 1.0f));
        if (fillW > 0.5f) {
            ctx.fillRectangle(toArea({sl.rect.x0 + 1.0f, sl.rect.y0 + 1.0f, sl.rect.x0 + 1.0f + fillW, sl.rect.y1 - 1.0f}, originX, originY),
                              kBtnActive, hover ? 0.98f : 0.85f);
        }
        float kx = sl.rect.x0 + 1.0f + fillW;
        ctx.fillRectangle(toArea({kx - 2.0f, sl.rect.y0 - 1.0f, kx + 2.0f, sl.rect.y1 + 1.0f}, originX, originY), kText, 0.96f);
    }

    // Toggle switches
    for (auto const& tg : layout.toggles) {
        bool hover = tg.rect.contains(mouseX, mouseY);
        ctx.fillRectangle(toArea(tg.rect, originX, originY), hover ? kBtnHover : kBtnBg, 0.55f);
        drawUiText(ctx, font, {tg.rect.x0 + 4.0f, tg.rect.y0 + 2.2f, tg.rect.x1 - 31.0f, tg.rect.y1},
                   originX, originY, tg.label, kText, 0.96f, 0.65f);

        geom::RectF pill{tg.rect.x1 - 26.0f, tg.rect.y0 + 2.0f, tg.rect.x1 - 3.0f, tg.rect.y1 - 2.0f};
        ctx.fillRectangle(toArea(pill, originX, originY), tg.checked ? kBtnSuccess : mce::Color(51, 65, 85), 0.95f);
        ctx.drawRectangle(toArea(pill, originX, originY), hover ? kBorderFocus : kBorder, 0.65f, 1);
        float knobX = tg.checked ? (pill.x1 - 8.5f) : (pill.x0 + 1.0f);
        ctx.fillRectangle(toArea({knobX, pill.y0 + 1.0f, knobX + 7.5f, pill.y1 - 1.0f}, originX, originY), kText, 0.98f);
    }

    // Section labels (Color, Cooldown, Priority, etc.)
    for (auto const& lb : layout.labels) {
        drawUiText(
            ctx, font, lb.rect, originX, originY, lb.text,
            lb.accent ? kBorderFocus : kMuted, 0.96f, 0.62f,
            lb.rightAlign ? ::ui::TextAlignment::Right : ::ui::TextAlignment::Left
        );
    }

    // Buttons & non-interactive value badges
    for (auto const& b : layout.buttons) {
        if (b.isBadge) {
            ctx.fillRectangle(toArea(b.rect, originX, originY), kPanelBg, 0.92f);
            ctx.drawRectangle(toArea(b.rect, originX, originY), kBorder, 0.45f, 1);
            drawUiText(
                ctx, font,
                {b.rect.x0 + 2.0f, b.rect.y0 + 2.2f, b.rect.x1 - 2.0f, b.rect.y1},
                originX, originY, b.label, kText, 0.92f, 0.64f, ::ui::TextAlignment::Center
            );
            continue;
        }
        bool hover = !b.disabled && b.rect.contains(mouseX, mouseY);
        if (b.swatchRgb != 0) {
            ctx.fillRectangle(toArea(b.rect, originX, originY), fromRgb(b.swatchRgb), 0.92f);
            ctx.drawRectangle(toArea(b.rect, originX, originY), b.active ? kText : (hover ? kBorderFocus : kPanelBg), 0.98f, b.active ? 2 : 1);
            if (!b.label.empty()) {
                drawUiText(ctx, font, {b.rect.x0 + 1.0f, b.rect.y0 + 2.0f, b.rect.x1 - 1.0f, b.rect.y1}, originX, originY, b.label, kPanelBg, 1.0f, 0.58f, ::ui::TextAlignment::Center);
            }
            continue;
        }
        mce::Color bg = b.disabled ? kBtnDisabled
                      : b.active   ? kBtnActive
                      : b.danger   ? kBtnDanger
                      : b.success  ? kBtnSuccess
                      : b.accent   ? kBtnAccent
                                   : (hover ? kBtnHover : kBtnBg);
        ctx.fillRectangle(toArea(b.rect, originX, originY), bg, b.disabled ? 0.60f : (hover ? 0.98f : 0.88f));
        ctx.drawRectangle(toArea(b.rect, originX, originY), b.disabled ? kBorder : ((b.active || hover) ? kBorderFocus : kBorder), b.disabled ? 0.35f : 0.70f, 1);
        bool leftAlign = b.id.starts_with("preset:") || b.id.starts_with("mstep:");
        drawUiText(
            ctx, font,
            {b.rect.x0 + 2.5f, b.rect.y0 + 2.2f, b.rect.x1 - 2.5f, b.rect.y1},
            originX, originY, b.label, b.disabled ? kMuted : kText, b.disabled ? 0.55f : 1.0f, 0.64f,
            leftAlign ? ::ui::TextAlignment::Left : ::ui::TextAlignment::Center
        );
    }

    // Text boxes (tight blinking cyan caret bar, click-to-caret, horizontal scrolling, Ctrl+A, [x] clear)
    editor::TextFieldId focusedNow = editor::TextFieldId::None;
    for (auto const& tb : layout.textBoxes) {
        if (tb.focused) {
            focusedNow = tb.field;
            break;
        }
    }
    if (focusedNow != mLastFocusedField) {
        mLastFocusedField = focusedNow;
        mCaretBlinkOrigin = now;
    }

    auto const elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - mCaretBlinkOrigin).count();
    bool const caretOn   = isForeground && ((elapsedMs < 0) || (((elapsedMs / 530) % 2) == 0));
    std::vector<geom::RectF> caretRects;

    constexpr float kInputFontScale = 0.68f;
    auto            inputMeasure    = makeMeasure(font, kInputFontScale);

    for (auto const& tb : layout.textBoxes) {
        bool hover = tb.rect.contains(mouseX, mouseY);
        drawUiText(ctx, font, {tb.rect.x0, tb.rect.y0 - 7.8f, tb.rect.x1, tb.rect.y0},
                   originX, originY, tb.caption, tb.focused ? kBorderFocus : kMuted, 0.96f, 0.62f);
        ctx.fillRectangle(toArea(tb.rect, originX, originY), tb.selectedAll ? mce::Color(30, 58, 110) : kPanelBg, 0.97f);
        ctx.drawRectangle(toArea(tb.rect, originX, originY), tb.focused ? kBorderFocus : (hover ? kText : kBorder), 0.90f, 1);

        float const textLeft  = tb.rect.x0 + 3.5f;
        float const textRight = tb.clearRect.width() > 1.0f ? (tb.clearRect.x0 - 2.0f) : (tb.rect.x1 - 3.0f);
        float const availW    = std::max(4.0f, textRight - textLeft);

        if (tb.focused) {
            auto slice = sliceTextForCaret(tb.value, tb.cursorByte, availW, kInputFontScale, inputMeasure);
            if (!slice.visibleText.empty()) {
                drawUiText(ctx, font, {textLeft, tb.rect.y0 + 2.2f, textRight, tb.rect.y1},
                           originX, originY, slice.visibleText, kText, 1.0f, kInputFontScale, ::ui::TextAlignment::Left, false);
            }
            if (caretOn) {
                // Compensate for the 1-font-px trailing glyph advance so the caret sits snugly beside the last character.
                float trimPx = (slice.caretOffsetPx > 0.5f) ? (0.85f * kInputFontScale) : 0.0f;
                float cx     = std::clamp(textLeft + std::max(0.0f, slice.caretOffsetPx - trimPx), textLeft, std::max(textLeft, textRight - 0.8f));
                caretRects.push_back({cx, tb.rect.y0 + 1.8f, cx + 0.75f, tb.rect.y1 - 1.8f});
            }
        } else {
            bool        isEmpty = tb.value.empty();
            std::string shown   = isEmpty ? tb.placeholder : tb.value;
            drawUiText(ctx, font, {textLeft, tb.rect.y0 + 2.2f, textRight, tb.rect.y1},
                       originX, originY, shown, isEmpty ? kMuted : kText, isEmpty ? 0.60f : 1.0f, kInputFontScale);
        }

        if (tb.clearRect.width() > 1.0f) {
            bool cHov = tb.clearRect.contains(mouseX, mouseY);
            ctx.fillRectangle(toArea(tb.clearRect, originX, originY), cHov ? kBtnDanger : kBtnBg, 0.80f);
            drawUiText(ctx, font, {tb.clearRect.x0, tb.clearRect.y0 + 1.5f, tb.clearRect.x1, tb.clearRect.y1},
                       originX, originY, "x", kText, 0.95f, 0.62f, ::ui::TextAlignment::Center);
        }
    }

    // Footer bar: show pinned action status (Save/Undo/Reload/Test) briefly, otherwise dynamic hover help!
    std::string hoverHint = isForeground ? editor::EditorModel::hoverHintAt(layout, mouseX, mouseY) : std::string{};
    bool        useHint   = !hoverHint.empty() && (now >= mStatusPinnedUntil);
    geom::RectF footer{layout.panel.x0 + 6.0f, layout.panel.y1 - 12.5f, layout.panel.x1 - 6.0f, layout.panel.y1 - 2.0f};
    drawUiText(ctx, font, footer, originX, originY, useHint ? hoverHint : status, useHint ? kBorderFocus : kGreen, 0.95f, 0.64f);

    ctx.flushText(0.016f, std::nullopt);

    // Draw blinking caret bars on top of flushed text (sleek 0.75px bar, no drop shadow).
    constexpr mce::Color kCaretColor(125, 224, 255);
    for (auto const& cr : caretRects) {
        ctx.fillRectangle(toArea(cr, originX, originY), kCaretColor, 0.98f);
    }
}

} // namespace radial
