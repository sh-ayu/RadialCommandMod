#include "mod/RadialMod.h"

#include "mod/RadialCommands.h"
#include "mod/RadialRenderer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ll/api/command/CommandRegistrar.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/client/ClientExitLevelEvent.h>
#include <ll/api/event/client/ClientJoinLevelEvent.h>
#include <ll/api/event/command/ClientCommandRegisterEvent.h>
#include <ll/api/event/input/KeyInputEvent.h>
#include <ll/api/event/input/MouseInputEvent.h>
#include <ll/api/event/render/UIRenderEvent.h>
#include <ll/api/event/world/ClientLevelTickEvent.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/TargetedBedrock.h>

#include <mc/client/game/ClientInstance.h>
#include <mc/client/game/IMinecraftGame.h>
#include <mc/client/gui/GuiData.h>
#include <mc/client/gui/screens/ScreenView.h>
#include <mc/client/multiplayer/ClientLevel.h>
#include <mc/client/options/IOptionRegistry.h>
#include <mc/client/player/LocalPlayer.h>
#include <mc/client/renderer/screen/MinecraftUIRenderContext.h>
#include <mc/deps/input/HIDController.h>
#include <mc/deps/input/Keyboard.h>
#include <mc/deps/input/MouseAction.h>
#include <mc/deps/input/RectangleArea.h>
#include <mc/network/ConnectionType.h>
#include <mc/network/GameConnectionInfo.h>
#include <mc/network/PacketSender.h>
#include <mc/network/packet/CommandRequestPacket.h>
#include <mc/network/packet/CommandRequestPacketPayload.h>
#include <mc/network/packet/TextPacket.h>
#include <mc/network/packet/TextPacketPayload.h>
#include <mc/server/commands/CommandContext.h>
#include <mc/server/commands/CommandOutput.h>
#include <mc/server/commands/CommandOutputMessageType.h>
#include <mc/server/commands/CurrentCmdVersion.h>
#include <mc/server/commands/PlayerCommandOrigin.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/storage/LevelData.h>

namespace radial {
namespace {

int64_t steadyNowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

bool detectSystemJapanese() noexcept {
    LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_JAPANESE;
}

bool isGameForeground() noexcept {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool isVkPhysicallyDown(int vk) noexcept {
    if (vk <= 0) return false;
    if (vk == kVkMenu) {
        return ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
            || ((GetAsyncKeyState(VK_LMENU) & 0x8000) != 0)
            || ((GetAsyncKeyState(VK_RMENU) & 0x8000) != 0);
    }
    if (vk == kVkShift) {
        return ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
            || ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0)
            || ((GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
    }
    if (vk == kVkControl) {
        return ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
            || ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0)
            || ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool isAltChordActive() noexcept {
    // Allow Shift+Alt so players can open the radial wheel while sneaking.
    return ((GetAsyncKeyState(VK_TAB) & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_RETURN) & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_F4) & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0);
}

int mouseActionToVk(char actionBtn) noexcept {
    if (actionBtn == MouseAction::ActionMiddle) return kVkMButton;
    if (actionBtn == MouseAction::ActionX1) return kVkXButton1;
    if (actionBtn == MouseAction::ActionX2) return kVkXButton2;
    return 0;
}

std::string trimAndSanitizeMessage(std::string_view sv, size_t maxBytes) {
    size_t b = 0, e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) {
        unsigned char c = static_cast<unsigned char>(sv[i]);
        if (c == '\r' || c == '\n' || c == '\t' || c < 0x20u || c == 0x7Fu) continue;
        out.push_back(static_cast<char>(c));
    }
    return truncateUtf8Bytes(out, maxBytes);
}

void showLocalChatFeedback(ClientInstance& client, std::string const& msg) noexcept {
    try {
        if (msg.empty()) return;
        auto gui = client.getGuiData();
        gui->displayClientMessage(msg, std::nullopt, false);
    } catch (...) {
    }
}

struct AtomicFlagGuard {
    std::atomic<bool>& flag;
    explicit AtomicFlagGuard(std::atomic<bool>& f) noexcept : flag(f) {
        flag.store(true, std::memory_order_release);
    }
    ~AtomicFlagGuard() noexcept { flag.store(false, std::memory_order_release); }
};

} // namespace

RadialMod& RadialMod::instance() noexcept {
    static RadialMod inst;
    return inst;
}

ResolvedLang RadialMod::currentLangLocked() const noexcept {
    return resolveUiLanguage(mConfig.language, mSystemIsJapanese);
}

void RadialMod::saveConfigAsyncSafe(RadialConfig const& snapshot) noexcept {
    try {
        if (!mConfigPath.empty()) {
            saveConfigFile(mConfigPath, snapshot);
        }
    } catch (...) {
    }
}

bool RadialMod::isSafeHudGameplayState(ClientInstance& client) noexcept {
    try {
        if (client.isLeavingGame() || client.isExitingLevel() || client.isDestroyingGame() || client.isShuttingDown()) {
            return false;
        }
        auto* player = client.getLocalPlayer();
        if (!player || !client.getMinecraftGame().isInGame()) return false;
        if (!client.isInWorldAndNotShowingAnyMenuScreens()) return false;
        if (client.isShowingMenu() || client.isShowingPauseScreen() || client.isShowingLoadingScreen()
            || client.isShowingDeathScreen() || client.isShowingServerForm() || client.isInBedScreen()) {
            return false;
        }
        std::string const top = client.getTopScreenName();
        return top == "hud_screen" || top == "hud_crosshair_screen";
    } catch (...) {
        return false;
    }
}

bool RadialMod::load() {
    mSelf = ll::mod::NativeMod::current();
    if (!mSelf) return false;

    mSystemIsJapanese = detectSystemJapanese();
    mShutdown.store(false, std::memory_order_release);
    mConfigPath = mSelf->getConfigDir() / "config.json";

    auto loaded = loadConfigFile(mConfigPath);
    {
        std::lock_guard lock(mMutex);
        mConfig = loaded.config;
        mVerboseLog.store(mConfig.verboseLog, std::memory_order_relaxed);
        mController.setConfig(mConfig, steadyNowMs());
    }

    if (!loaded.existed || loaded.corrupted) {
        if (loaded.corrupted) {
            mSelf->getLogger().warn(
                "RadialCommandMod: corrupt config backed up ({}); writing defaults to {}",
                loaded.warning,
                mConfigPath.string()
            );
        }
        if (!saveConfigFile(mConfigPath, loaded.config)) {
            mSelf->getLogger().warn("RadialCommandMod: failed to write default config to {}", mConfigPath.string());
        }
    } else if (!loaded.warning.empty()) {
        mSelf->getLogger().warn("RadialCommandMod: config warnings: {}", loaded.warning);
    }

    mSelf->getLogger().info(
        "RadialCommandMod loaded (holdKey={}, editorKey={}, lang={}, presets={}, config={})",
        keyCodeToName(loaded.config.activationKey),
        keyCodeToName(loaded.config.editorKey),
        uiLanguageToString(loaded.config.language),
        loaded.config.presets.size(),
        mConfigPath.string()
    );
    return true;
}

bool RadialMod::enable() {
    if (!mSelf) return false;
    mInHud.store(false, std::memory_order_release);
    mEditorKeyHeld.store(false, std::memory_order_release);
    mActKeyPolledDown.store(false, std::memory_order_release);
    mSuppressWheelUntilRelease.store(false, std::memory_order_release);
    mWasForeground.store(true, std::memory_order_release);
    mLastServerProbeMs.store(0, std::memory_order_release);
    mJoinReprobeUntilMs.store(0, std::memory_order_release);
    mPendingMoveReset.store(false, std::memory_order_release);

    mEditor.setCallbacks(
        [this](RadialConfig const& updated, std::string& msgOut) -> bool {
            RadialConfig clean = updated;
            sanitizeConfig(clean);
            if (!saveConfigFile(mConfigPath, clean)) {
                msgOut = "Failed to write " + mConfigPath.string();
                return false;
            }
            {
                std::lock_guard lock(mMutex);
                mConfig = clean;
                mVerboseLog.store(mConfig.verboseLog, std::memory_order_relaxed);
                mController.setConfig(mConfig, steadyNowMs());
            }
            return true;
        },
        [this](RadialConfig& reloadedOut, std::string& msgOut) -> bool {
            if (!reloadConfig(msgOut)) return false;
            std::lock_guard lock(mMutex);
            reloadedOut = mConfig;
            return true;
        },
        [this](RadialSlot const& slot) {
            std::lock_guard lock(mMutex);
            mController.enqueueSlotForTest(slot, mConfig, steadyNowMs(), currentLangLocked());
        }
    );
    if (!mEditor.installHooks()) {
        mSelf->getLogger().warn("RadialCommandMod: editor grabMouse hook unavailable; cursor may flicker in editor");
    }

    auto& bus = ll::event::EventBus::getInstance();

    mRenderListener = bus.emplaceListener<ll::event::render::AfterUIRenderEvent>(
        [this](auto& ev) { onAfterUiRender(ev); },
        ll::event::EventPriority::Low,
        mSelf
    );
    // Highest priority ensures cancelled events in editor/wheel never leak to other mods (e.g. LightweightHud K).
    mKeyListener = bus.emplaceListener<ll::event::input::KeyInputEvent>(
        [this](auto& ev) { onKeyInput(ev); },
        ll::event::EventPriority::Highest,
        mSelf
    );
    mMouseListener = bus.emplaceListener<ll::event::input::MouseInputEvent>(
        [this](auto& ev) { onMouseInput(ev); },
        ll::event::EventPriority::Highest,
        mSelf
    );
    mCommandListener = bus.emplaceListener<ll::event::command::ClientCommandRegisterEvent>(
        [this](auto&) {
            try {
                registerRadialCommands(mSelf);
            } catch (...) {
            }
        },
        ll::event::EventPriority::Low,
        mSelf
    );
    mJoinListener = bus.emplaceListener<ll::event::client::ClientJoinLevelEvent>(
        [this](auto&) { onWorldJoin(); },
        ll::event::EventPriority::Normal,
        mSelf
    );
    mExitListener = bus.emplaceListener<ll::event::client::ClientExitLevelEvent>(
        [this](auto&) { onWorldExit(); },
        ll::event::EventPriority::Normal,
        mSelf
    );
    mTickListener = bus.emplaceListener<ll::event::world::ClientLevelTickEvent>(
        [this](auto&) { onClientTick(); },
        ll::event::EventPriority::Normal,
        mSelf
    );

    if (!mRenderListener || !mKeyListener || !mMouseListener) {
        disable();
        return false;
    }

    // Release shutdown gate LAST after every listener & hook is ready.
    mShutdown.store(false, std::memory_order_release);
    mSelf->getLogger().info("RadialCommandMod enabled");
    return true;
}

bool RadialMod::disable() {
    // Ordered fail-closed teardown:
    // 1) Raise shutdown gate (release) so in-flight callbacks bail out cleanly.
    mShutdown.store(true, std::memory_order_release);
    mInHud.store(false, std::memory_order_release);

    // 2) Remove all EventBus listeners BEFORE unhooking.
    auto& bus = ll::event::EventBus::getInstance();
    for (auto* ptr : {&mRenderListener, &mKeyListener, &mMouseListener, &mCommandListener, &mJoinListener, &mExitListener, &mTickListener}) {
        if (*ptr) {
            bus.removeListener(*ptr);
            ptr->reset();
        }
    }

    // 3) Close editor (restoring cursor if we released it) & unhook grabMouse safely.
    ClientInstance* clientPtr = nullptr;
    try {
        if (auto ref = ll::service::getClientInstance()) clientPtr = &ref.value();
    } catch (...) {
    }
    mEditor.forceCloseOnDisable(clientPtr);
    mEditor.uninstallHooks();
    mEditor.clearCallbacks();

    // 4) Clear controller state under mutex.
    {
        std::lock_guard lock(mMutex);
        mController.cancelWheel();
        mController.clearPendingActions();
    }
    return true;
}

ServerIdentity RadialMod::probeServerIdentity(ClientInstance& client) noexcept {
    ServerIdentity id;
    try {
        if (client.isLeavingGame() || client.isExitingLevel() || client.isDestroyingGame() || client.isShuttingDown()) {
            return id;
        }
        auto* player = client.getLocalPlayer();
        bool  inGame = player && client.getMinecraftGame().isInGame();
        if (!inGame) {
            id.inWorld = false;
            return id;
        }
        id.inWorld = true;
        id.isRealm = client.isInRealm();

        bool hostingLocal = false;
        try {
            hostingLocal = client.getMinecraftGame().isHostingLocalDedicatedServer();
        } catch (...) {
        }

        if (auto connOpt = client.getGameConnectionInfo(); connOpt.has_value()) {
            auto const& conn = *connOpt;
            id.host          = conn.mUnresolvedUrl.get();
            id.ip            = conn.mHostIpAddress.get();
            id.port          = conn.mPort;
            id.serverName    = conn.mExternalServerName.get();

            bool typeLocal    = (conn.mType == Social::ConnectionType::Local);
            bool loopbackOnly = (id.host.empty() || isLoopbackHost(id.host))
                             && (id.ip.empty() || isLoopbackHost(id.ip))
                             && id.serverName.empty();

            if (id.isRealm) {
                id.isLocal = false;
            } else if (typeLocal || hostingLocal || loopbackOnly) {
                id.isLocal = true;
            } else {
                bool typeExternal = (conn.mType != Social::ConnectionType::Undefined);
                bool hasExtInfo   = !id.host.empty() || !id.ip.empty() || !id.serverName.empty();
                id.isLocal        = !(typeExternal || hasExtInfo);
            }
        } else {
            id.isLocal = !id.isRealm;
        }

        if (auto* level = client.getLevel()) {
            try {
                id.worldName = level->getLevelData().mLevelName.get();
            } catch (...) {
            }
        }
    } catch (...) {
    }
    return id;
}

void RadialMod::refreshServerIfDue(ClientInstance& client, int64_t nowMs, bool force) noexcept {
    try {
        int64_t prev     = mLastServerProbeMs.load(std::memory_order_relaxed);
        int64_t interval = (nowMs < mJoinReprobeUntilMs.load(std::memory_order_relaxed)) ? 100 : 500;
        if (!force && (nowMs - prev) < interval) return;
        mLastServerProbeMs.store(nowMs, std::memory_order_relaxed);

        ServerIdentity probed  = probeServerIdentity(client);
        bool           changed = false;
        std::string    presetName;
        {
            std::lock_guard lock(mMutex);
            changed = mController.updateServerIdentity(probed, mConfig, nowMs, currentLangLocked());
            int idx = mController.activePresetIndex();
            if (idx >= 0 && idx < static_cast<int>(mConfig.presets.size())) {
                presetName = mConfig.presets[static_cast<size_t>(idx)].name;
            }
        }
        if (changed && mSelf) {
            mSelf->getLogger().info(
                "RadialCommandMod: server '{}' -> preset '{}'",
                probed.displayLabel(),
                presetName
            );
        }
    } catch (...) {
    }
}

void RadialMod::onWorldJoin() noexcept {
    try {
        if (mShutdown.load(std::memory_order_acquire)) return;
        int64_t nowMs = steadyNowMs();
        mLastServerProbeMs.store(0, std::memory_order_relaxed);
        // Re-probe rapidly for 3 seconds because ClientJoinLevelEvent fires right before onPlayerLoaded finishes.
        mJoinReprobeUntilMs.store(nowMs + 3000, std::memory_order_relaxed);
    } catch (...) {
    }
}

void RadialMod::onWorldExit() noexcept {
    try {
        if (mShutdown.load(std::memory_order_acquire)) return;
        mInHud.store(false, std::memory_order_release);
        mEditor.requestClose(false);
        std::lock_guard lock(mMutex);
        mController.updateServerIdentity(ServerIdentity{}, mConfig, steadyNowMs(), currentLangLocked());
    } catch (...) {
    }
}

void RadialMod::onClientTick() noexcept {
    try {
        if (mShutdown.load(std::memory_order_acquire)) return;
        auto ref = ll::service::getClientInstance();
        if (!ref) return;
        auto& client = ref.value();

        if (mPendingMoveReset.exchange(false, std::memory_order_acq_rel)) {
            try {
                client.resetPlayerMovement();
            } catch (...) {
            }
        }

        drainDueActions(client, steadyNowMs());
    } catch (...) {
    }
}

void RadialMod::drainDueActions(ClientInstance& client, int64_t nowMs) noexcept {
    try {
        if (client.isLeavingGame() || client.isExitingLevel() || client.isDestroyingGame() || client.isShuttingDown()) {
            std::lock_guard lock(mMutex);
            mController.clearPendingActions();
            return;
        }
        std::vector<QueuedAction> due;
        {
            std::lock_guard lock(mMutex);
            due = mController.pollDueActions(nowMs);
        }
        for (auto const& act : due) {
            dispatchAction(client, act);
        }
    } catch (...) {
    }
}

bool RadialMod::executeBuiltinRcCommand(std::string_view rawCmd, ClientInstance& client) noexcept {
    try {
        std::string s(rawCmd);
        while (!s.empty() && (s.front() == '/' || std::isspace(static_cast<unsigned char>(s.front())))) {
            s.erase(s.begin());
        }
        if (!(s == "rc" || s.starts_with("rc "))) return false;

        std::istringstream iss(s);
        std::string        root, sub, arg;
        iss >> root >> sub;
        std::getline(iss >> std::ws, arg);

        if (sub.empty() || sub == "help") {
            showLocalChatFeedback(client, describeHelp());
        } else if (sub == "status") {
            showLocalChatFeedback(client, describeStatus());
        } else if (sub == "server") {
            showLocalChatFeedback(client, describeServer());
        } else if (sub == "presets") {
            showLocalChatFeedback(client, describePresets());
        } else if (sub == "editor") {
            requestOpenEditor();
        } else if (sub == "toggle") {
            bool on = toggleEnabled();
            showLocalChatFeedback(client, std::string("RadialCommandMod: ") + (on ? "ON" : "OFF"));
        } else if (sub == "reload") {
            std::string msg;
            reloadConfig(msg);
            showLocalChatFeedback(client, msg);
        } else if (sub == "reset") {
            resetConfig();
            showLocalChatFeedback(client, "RadialCommandMod: config reset to defaults");
        } else if (sub == "preset" && !arg.empty()) {
            std::string msg;
            selectPresetByName(arg, msg);
            showLocalChatFeedback(client, msg);
        } else if (sub == "bind" && !arg.empty()) {
            std::string msg;
            setActivationKeyByName(arg, msg);
            showLocalChatFeedback(client, msg);
        } else if (sub == "editorkey" && !arg.empty()) {
            std::string msg;
            setEditorKeyByName(arg, msg);
            showLocalChatFeedback(client, msg);
        } else if (sub == "mode" && !arg.empty()) {
            std::string msg;
            setActivationModeByName(arg, msg);
            showLocalChatFeedback(client, msg);
        } else if (sub == "lang" && !arg.empty()) {
            std::string msg;
            setLanguageByName(arg, msg);
            showLocalChatFeedback(client, msg);
        } else if (sub == "sensitivity" && !arg.empty()) {
            float v = setSensitivity(std::strtof(arg.c_str(), nullptr));
            showLocalChatFeedback(client, "RadialCommandMod: flickSensitivity = " + std::to_string(v));
        } else if (sub == "cooldown" && !arg.empty()) {
            int v = setGlobalCooldown(std::atoi(arg.c_str()));
            showLocalChatFeedback(client, "RadialCommandMod: globalCooldownMs = " + std::to_string(v) + "ms");
        } else {
            showLocalChatFeedback(client, describeHelp());
        }
        return true;
    } catch (...) {
        return false;
    }
}

void RadialMod::dispatchAction(ClientInstance& client, QueuedAction const& act) noexcept {
    try {
        if (client.isLeavingGame() || client.isExitingLevel() || client.isDestroyingGame() || client.isShuttingDown()) {
            return;
        }
        auto* player = client.getLocalPlayer();
        auto* level  = client.getLevel();
        if (!player || !level || !client.getMinecraftGame().isInGame()) return;

        // Build placeholder context from live player/server state.
        PlaceholderContext pctx;
        pctx.playerName = player->mName.get();
        if (pctx.playerName.empty()) pctx.playerName = player->getNameTag();
        {
            auto const& pos = player->getPosition();
            pctx.x          = static_cast<int>(std::floor(pos.x));
            pctx.y          = static_cast<int>(std::floor(pos.y));
            pctx.z          = static_cast<int>(std::floor(pos.z));
        }
        {
            std::lock_guard lock(mMutex);
            pctx.serverLabel = mController.currentServer().displayLabel();
            pctx.worldName   = mController.currentServer().worldName;
        }

        std::string expanded = expandPlaceholders(act.value, pctx);

        // Built-in /rc commands execute cleanly without network roundtrip.
        if ((act.type == ActionType::ClientCommand || act.type == ActionType::Command)
            && executeBuiltinRcCommand(expanded, client)) {
            return;
        }

        if (act.type == ActionType::Chat) {
            std::string clean = trimAndSanitizeMessage(expanded, 256);
            if (clean.empty()) return;
            auto& sender = client.getPacketSender();
            if (!sender.isInitialized()) return;
            std::string xuid = player->getXuid();
            auto        pkt  = TextPacketPayload::createChat(pctx.playerName, clean, std::nullopt, xuid, "");
            sender.sendToServer(pkt);
            if (mSelf && mVerboseLog.load(std::memory_order_relaxed)) {
                mSelf->getLogger().info("RadialCommandMod: sent chat '{}'", clean);
            }
            return;
        }

        if (act.type == ActionType::ClientCommand) {
            std::string body = trimAndSanitizeMessage(expanded, 256);
            while (!body.empty() && body.front() == '/') body.erase(body.begin());
            body = trimAndSanitizeMessage(body, 256);
            if (body.empty()) return;

            auto origin = std::make_unique<PlayerCommandOrigin>(*level, player->getOrCreateUniqueID());
            auto out    = ll::command::CommandRegistrar::getClientInstance().executeCommand(body, *origin);
            for (auto const& m : out.mMessages) {
                if (m.mMessageId.empty()) continue;
                std::string prefix = (m.mType == CommandOutputMessageType::Error) ? "\xC2\xA7" "c" : "";
                showLocalChatFeedback(client, prefix + m.mMessageId);
            }
            return;
        }

        if (act.type == ActionType::Command) {
            std::string cmd = trimAndSanitizeMessage(expanded, 256);
            while (!cmd.empty() && cmd.front() == '/') cmd.erase(cmd.begin());
            cmd = trimAndSanitizeMessage(cmd, 256);
            if (cmd.empty()) return;
            cmd.insert(cmd.begin(), '/');

            auto& sender = client.getPacketSender();
            if (!sender.isInitialized()) return;
            auto                        origin = std::make_unique<PlayerCommandOrigin>(*level, player->getOrCreateUniqueID());
            CommandContext              ctx(cmd, std::move(origin), static_cast<int>(CurrentCmdVersion::Latest));
            CommandRequestPacketPayload payload(ctx, false);
            CommandRequestPacket        pkt(std::move(payload));
            sender.sendToServer(pkt);
            if (mSelf && mVerboseLog.load(std::memory_order_relaxed)) {
                mSelf->getLogger().info("RadialCommandMod: sent command '{}'", cmd);
            }
            return;
        }
    } catch (...) {
        if (mSelf) {
            try {
                mSelf->getLogger().warn("RadialCommandMod: exception while dispatching action '{}'", act.value);
            } catch (...) {
            }
        }
    }
}

void RadialMod::onAfterUiRender(ll::event::render::AfterUIRenderEvent& ev) noexcept {
    try {
        if (mShutdown.load(std::memory_order_acquire)) return;

        auto ref = ll::service::getClientInstance();
        if (!ref) return;
        auto& client = ref.value();

        // Immediately drop mInHud on non-HUD screen passes (prevents chat/inventory misfire,
        // and suspends editor cleanly on Pause screen without discarding unsaved edits!).
        std::string const viewName = ev.screenView().getScreenName();
        bool const        safeHud  = isSafeHudGameplayState(client);
        if (viewName != "hud_screen") {
            if (!safeHud) {
                mInHud.store(false, std::memory_order_release);
                if (mEditor.isOpen()) {
                    mEditor.setSuspended(true);
                }
            }
            return;
        }

        int64_t nowMs = steadyNowMs();
        mInHud.store(safeHud, std::memory_order_release);
        mLastHudFrameMs.store(nowMs, std::memory_order_release);

        auto* player  = client.getLocalPlayer();
        bool  inWorld = player && client.getMinecraftGame().isInGame()
                     && !client.isLeavingGame() && !client.isExitingLevel();
        if (!inWorld) {
            mEditor.requestClose(false);
            mActKeyPolledDown.store(false, std::memory_order_relaxed);
            std::lock_guard lock(mMutex);
            mController.cancelWheel();
            return;
        }

        refreshServerIfDue(client, nowMs, false);

        auto& ctx  = ev.uiRenderContext();
        auto  clip = ctx.getFullClippingRectangle();
        float ox   = clip._x0;
        float oy   = clip._y0;
        float sw   = clip._x1 - clip._x0;
        float sh   = clip._y1 - clip._y0;
        if (!(sw > 0.0f && sh > 0.0f)) return;

        bool const foreground = isGameForeground();

        // Overlay editor rendering & cursor management (stays open across Alt-Tab / other windows!).
        mEditor.onHudRender(client, ctx, ox, oy, sw, sh, inWorld, safeHud, foreground);
        if (mEditor.isOpen()) {
            mSuppressWheelUntilRelease.store(true, std::memory_order_relaxed);
            std::lock_guard lock(mMutex);
            mController.cancelWheel();
            return;
        }
        // Suppress wheel on the exact frame the editor closes so held/residual keys never misfire.
        if (mEditor.consumeJustClosedFlag()) {
            mSuppressWheelUntilRelease.store(true, std::memory_order_relaxed);
            return;
        }

        bool const prevFg = mWasForeground.exchange(foreground, std::memory_order_relaxed);
        if (foreground && !prevFg) {
            // Focus just returned (e.g. via Alt+Tab): suppress wheel until activation key is released once!
            mSuppressWheelUntilRelease.store(true, std::memory_order_relaxed);
        }

        RadialViewSnapshot snap;
        RadialConfig       cfgCopy;
        ResolvedLang       lang = ResolvedLang::En;
        {
            std::lock_guard lock(mMutex);
            cfgCopy = mConfig;
            lang    = currentLangLocked();
            if (!safeHud || !foreground || !mConfig.enabled) {
                mActKeyPolledDown.store(false, std::memory_order_relaxed);
                if (mController.isPressTracked()) {
                    mController.cancelWheel();
                }
            } else {
                bool const physDown = isVkPhysicallyDown(mConfig.activationKey);
                bool const isAltBind = (mConfig.activationKey == kVkMenu
                                        || mConfig.activationKey == kVkLMenu
                                        || mConfig.activationKey == kVkRMenu);
                bool const altChord = isAltBind && isAltChordActive();
                bool const wasDown  = mActKeyPolledDown.exchange(physDown, std::memory_order_relaxed);

                if (!physDown) {
                    mSuppressWheelUntilRelease.store(false, std::memory_order_relaxed);
                }

                if (physDown && altChord) {
                    if (mController.isPressTracked()) mController.cancelWheel();
                    mSuppressWheelUntilRelease.store(true, std::memory_order_relaxed);
                } else if (mSuppressWheelUntilRelease.load(std::memory_order_relaxed)) {
                    // Wait cleanly until the key is released once.
                } else if (physDown && !wasDown) {
                    mController.onActivationDown(mConfig, nowMs, lang);
                } else if (!physDown && wasDown && mController.isPressTracked()) {
                    mController.onActivationUp(mConfig, nowMs, lang);
                }
                mController.update(mConfig, nowMs);
            }
            snap = mController.buildSnapshot(mConfig, nowMs);
        }

        if (client.getOptions().getHideHud()) return;

        if (snap.open) {
            RadialRenderer::renderWheel(ctx, ox, oy, sw, sh, snap, cfgCopy, lang);
        } else if (snap.toast.has_value()) {
            RadialRenderer::renderToast(ctx, ox, oy, sw, *snap.toast);
        }
    } catch (...) {
    }
}

void RadialMod::onKeyInput(ll::event::input::KeyInputEvent& ev) noexcept {
    try {
        if (mShutdown.load(std::memory_order_acquire)) return;
        if (mSynthesizingKeyUp.load(std::memory_order_acquire)) return;

        int  vk   = ev.keyCode();
        bool down = ev.isDown();

        int  actKey    = 0;
        int  editorKey = 0;
        bool enabled   = false;
        int  holdDel   = 0;
        {
            std::lock_guard lock(mMutex);
            actKey    = mConfig.activationKey;
            editorKey = mConfig.editorKey;
            enabled   = mConfig.enabled;
            holdDel   = mConfig.holdDelayMs;
        }

        // Always update editor key release BEFORE mEditor consumes events so mEditorKeyHeld never sticks!
        if (!down && editorKey > 0 && keyMatchesBinding(vk, editorKey)) {
            mEditorKeyHeld.store(false, std::memory_order_relaxed);
        }

        if (mEditor.onKeyInput(ev)) return;

        int64_t nowMs = steadyNowMs();
        bool    hudOk = mInHud.load(std::memory_order_acquire)
                     && (nowMs - mLastHudFrameMs.load(std::memory_order_acquire) < 350)
                     && isGameForeground();

        // Editor hotkey toggle (edge-triggered).
        if (editorKey > 0 && keyMatchesBinding(vk, editorKey)) {
            bool was = mEditorKeyHeld.exchange(down, std::memory_order_relaxed);
            if (down && !was && hudOk) {
                try {
                    auto&           states = ::Keyboard::_states();
                    auto&           ctrl   = ev.controller();
                    AtomicFlagGuard guard(mSynthesizingKeyUp);
                    for (int k = 1; k < 255; ++k) {
                        if (k == vk) continue;
                        if (states[k] != 0) ctrl.onKeyUp(k);
                    }
                } catch (...) {
                }
                requestOpenEditor();
                ev.cancel();
            }
            return;
        }

        // Activation hold/toggle key handling (for non-mouse keys that reach KeyInputEvent).
        if (!isMouseButtonVk(actKey) && keyMatchesBinding(vk, actKey)) {
            if (down) {
                if (!enabled || !hudOk) return;
                bool const isAltBind = (actKey == kVkMenu || actKey == kVkLMenu || actKey == kVkRMenu);
                if (isAltBind && isAltChordActive()) {
                    mSuppressWheelUntilRelease.store(true, std::memory_order_relaxed);
                    return;
                }
                if (mSuppressWheelUntilRelease.load(std::memory_order_relaxed)) return;
                mActKeyPolledDown.store(true, std::memory_order_relaxed);
                std::lock_guard lock(mMutex);
                if (!mController.isPressTracked() || mConfig.activationMode == ActivationMode::Toggle) {
                    mController.onActivationDown(mConfig, nowMs, currentLangLocked());
                }
                // Only cancel the key when holdDelayMs == 0 or wheel is open (allows short tap passthrough when holdDelayMs > 0).
                if (holdDel == 0 || mController.isWheelOpen()) {
                    ev.cancel();
                }
            } else {
                mActKeyPolledDown.store(false, std::memory_order_relaxed);
                mSuppressWheelUntilRelease.store(false, std::memory_order_relaxed);
                std::lock_guard lock(mMutex);
                if (mController.isPressTracked()) {
                    bool wasOpen = mController.isWheelOpen();
                    mController.onActivationUp(mConfig, nowMs, currentLangLocked());
                    if (wasOpen) ev.cancel();
                }
            }
            return;
        }

        // Keys while wheel is open (Esc, Backspace, 1..9 preset switch).
        std::lock_guard lock(mMutex);
        if (!mController.isWheelOpen() || !down) return;

        if (vk == kVkEscape) {
            mController.cancelWheel();
            ev.cancel();
            return;
        }
        if (vk == kVkBack) {
            mController.onBackOrCancel(nowMs);
            ev.cancel();
            return;
        }
        if (vk >= '1' && vk <= '9') {
            if (mController.onNumberKey(vk - '0', mConfig, nowMs, currentLangLocked())) {
                ev.cancel();
            }
            return;
        }
    } catch (...) {
    }
}

void RadialMod::onMouseInput(ll::event::input::MouseInputEvent& ev) noexcept {
    try {
        if (mShutdown.load(std::memory_order_acquire)) return;
        if (mEditor.onMouseInput(ev)) return;

        int64_t nowMs = steadyNowMs();
        bool    hudOk = mInHud.load(std::memory_order_acquire)
                     && (nowMs - mLastHudFrameMs.load(std::memory_order_acquire) < 350)
                     && isGameForeground();

        char  btn  = ev.actionButtonId();
        schar data = ev.buttonData();

        std::lock_guard lock(mMutex);

        if (isMouseButtonVk(mConfig.activationKey)) {
            int mv = mouseActionToVk(btn);
            if (mv != 0 && mv == mConfig.activationKey) {
                if (data == MouseAction::DataDown) {
                    if (!mConfig.enabled || !hudOk) return;
                    if (mSuppressWheelUntilRelease.load(std::memory_order_relaxed)) return;
                    mActKeyPolledDown.store(true, std::memory_order_relaxed);
                    if (!mController.isPressTracked() || mConfig.activationMode == ActivationMode::Toggle) {
                        mController.onActivationDown(mConfig, nowMs, currentLangLocked());
                    }
                    if (mConfig.holdDelayMs == 0 || mController.isWheelOpen()) ev.cancel();
                } else if (data == MouseAction::DataUp) {
                    mActKeyPolledDown.store(false, std::memory_order_relaxed);
                    mSuppressWheelUntilRelease.store(false, std::memory_order_relaxed);
                    if (mController.isPressTracked()) {
                        mController.onActivationUp(mConfig, nowMs, currentLangLocked());
                    }
                }
                return;
            }
        }

        if (!mController.isCapturingInput()) return;

        if (btn == MouseAction::ActionMove) {
            float dx = static_cast<float>(ev.dx());
            float dy = static_cast<float>(ev.dy());
            if (dx != 0.0f || dy != 0.0f) {
                mController.onMouseDelta(dx, dy, mConfig, nowMs);
            }
            ev.cancel();
            return;
        }

        if (btn == MouseAction::ActionWheel) {
            if (data != 0) {
                mController.onMouseWheel(data > 0 ? 1 : -1, mConfig, nowMs, currentLangLocked());
            }
            ev.cancel();
            return;
        }

        // Never cancel DataUp for left/right buttons so held mining/blocking releases cleanly!
        if (btn == MouseAction::ActionLeft) {
            if (data == MouseAction::DataDown) {
                mController.onLeftClick(mConfig, nowMs, currentLangLocked());
                ev.cancel();
            }
            return;
        }

        if (btn == MouseAction::ActionRight) {
            if (data == MouseAction::DataDown) {
                mController.onBackOrCancel(nowMs);
                ev.cancel();
            }
            return;
        }
    } catch (...) {
    }
}

// ============================================================================
// Client commands (/rc)
// ============================================================================

std::string RadialMod::describeHelp() const {
    return "RadialCommandMod commands:\n"
           "  /rc status                - Show active preset, keybinds & settings\n"
           "  /rc editor                - Open in-game visual GUI editor (or press F12)\n"
           "  /rc server                - Show detected server/world info & auto-match rule\n"
           "  /rc presets               - List all presets\n"
           "  /rc preset <id|name|auto> - Switch preset manually or restore auto\n"
           "  /rc bind <key>            - Change wheel key (Alt, Tab, R, V, G, Mouse4...)\n"
           "  /rc editorkey <key|None>  - Change editor hotkey (default: F12)\n"
           "  /rc mode <hold|toggle>    - Switch between Hold and Toggle wheel mode\n"
           "  /rc lang <auto|ja|en>     - Set UI language\n"
           "  /rc sensitivity <0.2..4>  - Adjust flick sensitivity\n"
           "  /rc cooldown <ms>         - Set global cooldown in milliseconds\n"
           "  /rc toggle                - Enable/disable radial menu\n"
           "  /rc reload | reset        - Reload config.json or reset defaults";
}

std::string RadialMod::describeStatus() {
    std::lock_guard lock(mMutex);
    int         idx   = mController.activePresetIndex();
    std::string pName = (idx >= 0 && idx < static_cast<int>(mConfig.presets.size()))
                          ? mConfig.presets[static_cast<size_t>(idx)].name
                          : "<none>";
    std::ostringstream oss;
    oss << "RadialCommandMod: " << (mConfig.enabled ? "ON" : "OFF")
        << " | mode=" << activationModeToString(mConfig.activationMode)
        << " | key=" << keyCodeToName(mConfig.activationKey)
        << " | editorKey=" << keyCodeToName(mConfig.editorKey)
        << " | lang=" << uiLanguageToString(mConfig.language)
        << " | preset=" << pName << (mController.isManualOverride() ? " (manual)" : " (auto)")
        << " | sens=" << mConfig.flickSensitivity
        << " | cd=" << mConfig.globalCooldownMs << "ms"
        << " | server=" << mController.currentServer().displayLabel()
        << " | config=" << mConfigPath.string();
    return oss.str();
}

std::string RadialMod::describeServer() {
    if (auto ref = ll::service::getClientInstance()) {
        refreshServerIfDue(ref.value(), steadyNowMs(), true);
    }
    std::lock_guard lock(mMutex);
    auto const& s     = mController.currentServer();
    auto        match = matchPresetForServer(mConfig, s);
    std::ostringstream oss;
    oss << "Server Info: inWorld=" << (s.inWorld ? "yes" : "no")
        << " | scope=" << (s.isLocal ? "local" : (s.isRealm ? "realm" : "external"))
        << " | host=" << (s.host.empty() ? "-" : s.host)
        << " | ip=" << (s.ip.empty() ? "-" : s.ip)
        << " | port=" << s.port
        << " | serverName=" << (s.serverName.empty() ? "-" : s.serverName)
        << " | worldName=" << (s.worldName.empty() ? "-" : s.worldName)
        << " | matchedPreset=" << match.matchedPresetId
        << (match.matchedRule ? " (rule)" : " (default)");
    return oss.str();
}

std::string RadialMod::describePresets() {
    std::lock_guard lock(mMutex);
    std::ostringstream oss;
    oss << "Radial presets (" << mConfig.presets.size() << "):";
    int active = mController.activePresetIndex();
    for (size_t i = 0; i < mConfig.presets.size(); ++i) {
        auto const& p = mConfig.presets[i];
        oss << "\n  [" << (i + 1) << "] " << p.name << " (id=" << p.id
            << ", slots=" << p.slots.size()
            << ", rules=" << p.serverRules.size() << ")";
        if (static_cast<int>(i) == active) oss << "  <-- ACTIVE";
    }
    return oss.str();
}

bool RadialMod::toggleEnabled() {
    RadialConfig snap;
    bool         nowOn = false;
    {
        std::lock_guard lock(mMutex);
        mConfig.enabled = !mConfig.enabled;
        nowOn           = mConfig.enabled;
        if (!nowOn) mController.cancelWheel();
        snap = mConfig;
    }
    saveConfigAsyncSafe(snap);
    return nowOn;
}

void RadialMod::requestOpenEditor() {
    RadialConfig   cfgCopy;
    ServerIdentity srvCopy;
    int            activeIdx = 0;
    {
        std::lock_guard lock(mMutex);
        mController.cancelWheel();
        cfgCopy   = mConfig;
        srvCopy   = mController.currentServer();
        activeIdx = mController.activePresetIndex();
    }
    mPendingMoveReset.store(true, std::memory_order_release);
    mEditor.requestOpen(cfgCopy, srvCopy, activeIdx, mSystemIsJapanese);
}

bool RadialMod::reloadConfig(std::string& messageOut) {
    auto loaded = loadConfigFile(mConfigPath);
    if (!loaded.existed) {
        messageOut = "RadialCommandMod: config.json not found (" + mConfigPath.string() + ")";
        return false;
    }
    if (loaded.corrupted) {
        messageOut = "RadialCommandMod: invalid config.json (" + loaded.warning + "); kept current settings";
        return false;
    }
    {
        std::lock_guard lock(mMutex);
        mConfig = loaded.config;
        mVerboseLog.store(mConfig.verboseLog, std::memory_order_relaxed);
        mController.setConfig(mConfig, steadyNowMs());
    }
    messageOut = "RadialCommandMod: config reloaded (" + std::to_string(loaded.config.presets.size()) + " presets)";
    if (!loaded.warning.empty()) messageOut += " [warnings: " + loaded.warning + "]";
    return true;
}

void RadialMod::resetConfig() {
    RadialConfig snap;
    {
        std::lock_guard lock(mMutex);
        mConfig = makeDefaultConfig();
        mVerboseLog.store(mConfig.verboseLog, std::memory_order_relaxed);
        mController.setConfig(mConfig, steadyNowMs());
        snap = mConfig;
    }
    saveConfigAsyncSafe(snap);
}

bool RadialMod::selectPresetByName(std::string_view nameOrId, std::string& messageOut) {
    std::lock_guard lock(mMutex);
    int64_t      nowMs = steadyNowMs();
    ResolvedLang lang  = currentLangLocked();
    if (nameOrId == "auto" || nameOrId == "AUTO") {
        mController.selectPreset(-1, mConfig, nowMs, false, lang);
        int idx    = mController.activePresetIndex();
        messageOut = "RadialCommandMod: restored auto preset -> "
                   + mConfig.presets[static_cast<size_t>(idx)].name;
        return true;
    }
    int idx = findPresetIndex(mConfig, nameOrId);
    if (idx < 0) {
        messageOut = "RadialCommandMod: preset not found: " + std::string(nameOrId);
        return false;
    }
    mController.selectPreset(idx, mConfig, nowMs, true, lang);
    messageOut = "RadialCommandMod: active preset -> " + mConfig.presets[static_cast<size_t>(idx)].name;
    return true;
}

bool RadialMod::setActivationKeyByName(std::string_view keyName, std::string& messageOut) {
    auto vk = parseKeyName(keyName);
    if (!vk || *vk <= 0) {
        messageOut = "RadialCommandMod: unknown key '" + std::string(keyName)
                   + "' (examples: Alt, Tab, R, V, G, CapsLock, Mouse4, Mouse5)";
        return false;
    }
    RadialConfig snap;
    {
        std::lock_guard lock(mMutex);
        mConfig.activationKey = *vk;
        snap                  = mConfig;
    }
    saveConfigAsyncSafe(snap);
    messageOut = "RadialCommandMod: wheel key set to " + keyCodeToName(*vk);
    return true;
}

bool RadialMod::setEditorKeyByName(std::string_view keyName, std::string& messageOut) {
    auto vk = parseKeyName(keyName);
    if (!vk || *vk < 0) {
        messageOut = "RadialCommandMod: unknown editor key '" + std::string(keyName)
                   + "' (examples: F12, F6, Insert, None)";
        return false;
    }
    RadialConfig snap;
    {
        std::lock_guard lock(mMutex);
        mConfig.editorKey = *vk;
        snap              = mConfig;
    }
    saveConfigAsyncSafe(snap);
    messageOut = "RadialCommandMod: editor key set to " + keyCodeToName(*vk);
    return true;
}

bool RadialMod::setActivationModeByName(std::string_view modeName, std::string& messageOut) {
    auto mode = parseActivationMode(modeName);
    if (!mode) {
        messageOut = "RadialCommandMod: unknown mode '" + std::string(modeName) + "' (use 'hold' or 'toggle')";
        return false;
    }
    RadialConfig snap;
    {
        std::lock_guard lock(mMutex);
        mConfig.activationMode = *mode;
        sanitizeConfig(mConfig);
        snap = mConfig;
    }
    saveConfigAsyncSafe(snap);
    messageOut = "RadialCommandMod: mode set to " + std::string(activationModeToString(*mode));
    return true;
}

bool RadialMod::setLanguageByName(std::string_view langName, std::string& messageOut) {
    auto lang = parseUiLanguage(langName);
    if (!lang) {
        messageOut = "RadialCommandMod: unknown language '" + std::string(langName) + "' (use 'auto', 'ja', or 'en')";
        return false;
    }
    RadialConfig snap;
    {
        std::lock_guard lock(mMutex);
        mConfig.language = *lang;
        snap             = mConfig;
    }
    saveConfigAsyncSafe(snap);
    messageOut = "RadialCommandMod: language set to " + std::string(uiLanguageToString(*lang));
    return true;
}

float RadialMod::setSensitivity(float value) {
    RadialConfig snap;
    float        out = value;
    {
        std::lock_guard lock(mMutex);
        mConfig.flickSensitivity = value;
        sanitizeConfig(mConfig);
        out  = mConfig.flickSensitivity;
        snap = mConfig;
    }
    saveConfigAsyncSafe(snap);
    return out;
}

int RadialMod::setGlobalCooldown(int ms) {
    RadialConfig snap;
    int          out = ms;
    {
        std::lock_guard lock(mMutex);
        mConfig.globalCooldownMs = ms;
        sanitizeConfig(mConfig);
        out  = mConfig.globalCooldownMs;
        snap = mConfig;
    }
    saveConfigAsyncSafe(snap);
    return out;
}

} // namespace radial
