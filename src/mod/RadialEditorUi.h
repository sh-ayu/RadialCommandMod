#pragma once

#include "core/EditorModel.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

class ClientInstance;
class MinecraftUIRenderContext;

namespace ll::event::inline input {
class KeyInputEvent;
class MouseInputEvent;
} // namespace ll::event::inline input

namespace radial {

class RadialEditorUi {
public:
    using SaveCallback   = std::function<bool(RadialConfig const&, std::string&)>;
    using ReloadCallback = std::function<bool(RadialConfig&, std::string&)>;
    using TestCallback   = std::function<void(RadialSlot const&)>;

    RadialEditorUi();
    ~RadialEditorUi() = default; // Never call unhook() from static destructor; RadialMod::disable() owns teardown.

    bool installHooks() noexcept;
    void uninstallHooks() noexcept;
    void clearCallbacks();

    void setCallbacks(SaveCallback onSave, ReloadCallback onReload, TestCallback onTest);

    void requestOpen(RadialConfig const& cfg, ServerIdentity const& server, int activePresetIdx, bool systemIsJapanese);
    void requestClose(bool regrabMouse = true) noexcept;
    // Direct close helper called during mod disable() on teardown.
    void forceCloseOnDisable(ClientInstance* client) noexcept;

    bool isOpen() const noexcept { return mActive.load(std::memory_order_acquire); }
    bool isSuspended() const noexcept { return mSuspended.load(std::memory_order_acquire); }
    void setSuspended(bool suspended) noexcept;
    bool consumeJustClosedFlag() noexcept { return mJustClosed.exchange(false, std::memory_order_acq_rel); }

    // Called from KeyInputEvent / MouseInputEvent (Highest priority). Returns true if consumed.
    bool onKeyInput(ll::event::input::KeyInputEvent& ev) noexcept;
    bool onMouseInput(ll::event::input::MouseInputEvent& ev) noexcept;

    // Called from HUD AfterUIRenderEvent on the UI thread.
    void onHudRender(
        ClientInstance&           client,
        MinecraftUIRenderContext& ctx,
        float                     originX,
        float                     originY,
        float                     screenW,
        float                     screenH,
        bool                      inWorld,
        bool                      safeHud,
        bool                      isForeground
    ) noexcept;

private:
    struct PendingKey {
        int         vk    = 0;
        bool        shift = false;
        bool        ctrl  = false;
        std::string typedUtf8;
    };

    void keepCursorVisible(ClientInstance& client, std::chrono::steady_clock::time_point now) noexcept;
    void renderPanel(
        MinecraftUIRenderContext&             ctx,
        float                                 originX,
        float                                 originY,
        float                                 screenW,
        float                                 screenH,
        float                                 mouseX,
        float                                 mouseY,
        bool                                  isForeground,
        std::chrono::steady_clock::time_point now
    ) noexcept;

    std::atomic<bool>    mActive{false};
    std::atomic<bool>    mSuspended{false};
    std::atomic<bool>    mOpenRequested{false};
    std::atomic<bool>    mClosePending{false};
    std::atomic<bool>    mRegrabOnClose{true};
    std::atomic<bool>    mJustClosed{false};
    std::atomic<bool>    mWeReleasedMouse{false};
    std::atomic<int64_t> mLastFrameNs{0};
    std::atomic<bool>    mLeftClickPending{false};
    std::atomic<bool>    mLeftButtonHeld{false};
    std::atomic<int>     mWheelDelta{0};
    std::atomic<int>     mCapturedMouseBtn{0};
    std::atomic<bool>    mAltWasDownForCapture{false};

    bool                                  mHookInstalled = false;
    bool                                  mWasForeground = true;
    std::chrono::steady_clock::time_point mLastRelease{};
    std::chrono::steady_clock::time_point mCaretBlinkOrigin{};
    std::chrono::steady_clock::time_point mLastClickTime{};
    std::chrono::steady_clock::time_point mStatusPinnedUntil{};
    float                                 mLastClickX       = -1000.0f;
    float                                 mLastClickY       = -1000.0f;
    editor::TextFieldId                   mLastFocusedField = editor::TextFieldId::None;

    std::mutex              mMutex;
    editor::EditorModel     mModel;
    RadialConfig            mPendingConfig;
    ServerIdentity          mPendingServer;
    int                     mPendingPresetIdx   = 0;
    bool                    mPendingSystemIsJa  = false;
    std::vector<PendingKey> mKeyQueue;
    SaveCallback            mOnSave;
    ReloadCallback          mOnReload;
    TestCallback            mOnTest;
};

} // namespace radial
