#pragma once

#include "core/Localization.h"
#include "core/RadialConfig.h"
#include "core/RadialController.h"
#include "core/ServerMatcher.h"
#include "mod/RadialEditorUi.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include <ll/api/event/ListenerBase.h>

class ClientInstance;

namespace ll::mod {
class NativeMod;
}

namespace ll::event::inline render {
class AfterUIRenderEvent;
}

namespace ll::event::inline input {
class KeyInputEvent;
class MouseInputEvent;
}

namespace radial {

class RadialMod {
public:
    static RadialMod& instance() noexcept;

    bool load();
    bool enable();
    bool disable();

    // --- Client command (/rc) operations ---
    std::string describeHelp() const;
    std::string describeStatus();
    std::string describeServer();
    std::string describePresets();

    bool  toggleEnabled();
    void  requestOpenEditor();
    bool  reloadConfig(std::string& messageOut);
    void  resetConfig();
    bool  selectPresetByName(std::string_view nameOrId, std::string& messageOut);
    bool  setActivationKeyByName(std::string_view keyName, std::string& messageOut);
    bool  setEditorKeyByName(std::string_view keyName, std::string& messageOut);
    bool  setActivationModeByName(std::string_view modeName, std::string& messageOut);
    bool  setLanguageByName(std::string_view langName, std::string& messageOut);
    float setSensitivity(float value);
    int   setGlobalCooldown(int ms);

private:
    RadialMod() = default;

    void onAfterUiRender(ll::event::render::AfterUIRenderEvent& ev) noexcept;
    void onKeyInput(ll::event::input::KeyInputEvent& ev) noexcept;
    void onMouseInput(ll::event::input::MouseInputEvent& ev) noexcept;
    void onWorldJoin() noexcept;
    void onWorldExit() noexcept;
    void onClientTick() noexcept;

    static bool    isSafeHudGameplayState(ClientInstance& client) noexcept;
    ServerIdentity probeServerIdentity(ClientInstance& client) noexcept;
    void           refreshServerIfDue(ClientInstance& client, int64_t nowMs, bool force) noexcept;
    void           drainDueActions(ClientInstance& client, int64_t nowMs) noexcept;
    void           dispatchAction(ClientInstance& client, QueuedAction const& act) noexcept;
    bool           executeBuiltinRcCommand(std::string_view rawCmd, ClientInstance& client) noexcept;
    ResolvedLang   currentLangLocked() const noexcept;
    void           saveConfigAsyncSafe(RadialConfig const& snapshot) noexcept;

    std::shared_ptr<ll::mod::NativeMod> mSelf;
    std::filesystem::path               mConfigPath;
    bool                                mSystemIsJapanese = false;

    std::atomic<bool>    mShutdown{true};
    std::atomic<bool>    mVerboseLog{false};
    std::atomic<bool>    mInHud{false};
    std::atomic<int64_t> mLastHudFrameMs{0};
    std::atomic<int64_t> mLastServerProbeMs{0};
    std::atomic<int64_t> mJoinReprobeUntilMs{0};
    std::atomic<bool>    mEditorKeyHeld{false};
    std::atomic<bool>    mActKeyPolledDown{false};
    std::atomic<bool>    mSuppressWheelUntilRelease{false};
    std::atomic<bool>    mWasForeground{true};
    std::atomic<bool>    mSynthesizingKeyUp{false};
    std::atomic<bool>    mPendingMoveReset{false};

    std::mutex       mMutex;
    RadialConfig     mConfig;
    RadialController mController;
    RadialEditorUi   mEditor;

    ll::event::ListenerPtr mRenderListener;
    ll::event::ListenerPtr mKeyListener;
    ll::event::ListenerPtr mMouseListener;
    ll::event::ListenerPtr mCommandListener;
    ll::event::ListenerPtr mJoinListener;
    ll::event::ListenerPtr mExitListener;
    ll::event::ListenerPtr mTickListener;
};

} // namespace radial
