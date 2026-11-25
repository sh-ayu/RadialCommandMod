#include "mod/RadialCommands.h"

#include "mod/RadialMod.h"

#include <string>

#include <ll/api/command/CommandHandle.h>
#include <ll/api/command/CommandRegistrar.h>

#include <mc/server/commands/CommandOrigin.h>
#include <mc/server/commands/CommandOutput.h>
#include <mc/server/commands/CommandPermissionLevel.h>

namespace radial {

struct StringArgParam {
    std::string value;
};

struct FloatArgParam {
    float value;
};

struct IntArgParam {
    int value;
};

void registerRadialCommands(std::weak_ptr<ll::mod::Mod> weakMod) {
    auto& registrar = ll::command::CommandRegistrar::getClientInstance();

    auto& cmd = registrar.getOrCreateCommand(
        "rc",
        "RadialCommandMod: radial command & quick-chat wheel controls",
        CommandPermissionLevel::Any,
        CommandFlagValue::NotCheat,
        weakMod
    );

    cmd.overload(weakMod).execute([](CommandOrigin const&, CommandOutput& out) {
        out.success("{}", RadialMod::instance().describeHelp());
    });

    cmd.overload(weakMod).text("help").execute([](CommandOrigin const&, CommandOutput& out) {
        out.success("{}", RadialMod::instance().describeHelp());
    });

    cmd.overload(weakMod).text("status").execute([](CommandOrigin const&, CommandOutput& out) {
        out.success("{}", RadialMod::instance().describeStatus());
    });

    cmd.overload(weakMod).text("server").execute([](CommandOrigin const&, CommandOutput& out) {
        out.success("{}", RadialMod::instance().describeServer());
    });

    cmd.overload(weakMod).text("presets").execute([](CommandOrigin const&, CommandOutput& out) {
        out.success("{}", RadialMod::instance().describePresets());
    });

    cmd.overload(weakMod).text("toggle").execute([](CommandOrigin const&, CommandOutput& out) {
        bool on = RadialMod::instance().toggleEnabled();
        out.success("RadialCommandMod: {}", on ? "ON" : "OFF");
    });

    cmd.overload(weakMod).text("editor").execute([](CommandOrigin const&, CommandOutput& out) {
        RadialMod::instance().requestOpenEditor();
        out.success("RadialCommandMod: opening in-game visual editor...");
    });

    cmd.overload(weakMod).text("reload").execute([](CommandOrigin const&, CommandOutput& out) {
        std::string msg;
        if (RadialMod::instance().reloadConfig(msg)) out.success("{}", msg);
        else out.error("{}", msg);
    });

    cmd.overload(weakMod).text("reset").execute([](CommandOrigin const&, CommandOutput& out) {
        RadialMod::instance().resetConfig();
        out.success("RadialCommandMod: config reset to defaults");
    });

    cmd.overload<StringArgParam>(weakMod).text("preset").required("value").execute(
        [](CommandOrigin const&, CommandOutput& out, StringArgParam const& p) {
            std::string msg;
            if (RadialMod::instance().selectPresetByName(p.value, msg)) out.success("{}", msg);
            else out.error("{}", msg);
        }
    );

    cmd.overload<StringArgParam>(weakMod).text("bind").required("value").execute(
        [](CommandOrigin const&, CommandOutput& out, StringArgParam const& p) {
            std::string msg;
            if (RadialMod::instance().setActivationKeyByName(p.value, msg)) out.success("{}", msg);
            else out.error("{}", msg);
        }
    );

    cmd.overload<StringArgParam>(weakMod).text("editorkey").required("value").execute(
        [](CommandOrigin const&, CommandOutput& out, StringArgParam const& p) {
            std::string msg;
            if (RadialMod::instance().setEditorKeyByName(p.value, msg)) out.success("{}", msg);
            else out.error("{}", msg);
        }
    );

    cmd.overload<StringArgParam>(weakMod).text("mode").required("value").execute(
        [](CommandOrigin const&, CommandOutput& out, StringArgParam const& p) {
            std::string msg;
            if (RadialMod::instance().setActivationModeByName(p.value, msg)) out.success("{}", msg);
            else out.error("{}", msg);
        }
    );

    cmd.overload<StringArgParam>(weakMod).text("lang").required("value").execute(
        [](CommandOrigin const&, CommandOutput& out, StringArgParam const& p) {
            std::string msg;
            if (RadialMod::instance().setLanguageByName(p.value, msg)) out.success("{}", msg);
            else out.error("{}", msg);
        }
    );

    cmd.overload<FloatArgParam>(weakMod).text("sensitivity").required("value").execute(
        [](CommandOrigin const&, CommandOutput& out, FloatArgParam const& p) {
            float v = RadialMod::instance().setSensitivity(p.value);
            out.success("RadialCommandMod: flickSensitivity = {:.2f}", v);
        }
    );

    cmd.overload<IntArgParam>(weakMod).text("cooldown").required("value").execute(
        [](CommandOrigin const&, CommandOutput& out, IntArgParam const& p) {
            int v = RadialMod::instance().setGlobalCooldown(p.value);
            out.success("RadialCommandMod: globalCooldownMs = {}ms", v);
        }
    );
}

} // namespace radial
