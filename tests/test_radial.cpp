#include "core/EditorModel.h"
#include "core/Localization.h"
#include "core/RadialConfig.h"
#include "core/RadialController.h"
#include "core/RadialGeometry.h"
#include "core/ServerMatcher.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

using namespace radial;

namespace {

int gTestsPassed = 0;

#define CHECK(expr)                                                                                           \
    do {                                                                                                      \
        if (!(expr)) {                                                                                        \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " : " << #expr << "\n";                  \
            return 1;                                                                                         \
        }                                                                                                     \
        ++gTestsPassed;                                                                                       \
    } while (0)

bool isValidUtf8(std::string_view sv) {
    size_t i = 0;
    while (i < sv.size()) {
        unsigned char b0 = static_cast<unsigned char>(sv[i]);
        if (b0 < 0x80u) {
            ++i;
        } else if ((b0 & 0xE0u) == 0xC0u) {
            if (i + 1 >= sv.size() || (static_cast<unsigned char>(sv[i + 1]) & 0xC0u) != 0x80u) return false;
            i += 2;
        } else if ((b0 & 0xF0u) == 0xE0u) {
            if (i + 2 >= sv.size()
                || (static_cast<unsigned char>(sv[i + 1]) & 0xC0u) != 0x80u
                || (static_cast<unsigned char>(sv[i + 2]) & 0xC0u) != 0x80u) return false;
            i += 3;
        } else if ((b0 & 0xF8u) == 0xF0u) {
            if (i + 3 >= sv.size()
                || (static_cast<unsigned char>(sv[i + 1]) & 0xC0u) != 0x80u
                || (static_cast<unsigned char>(sv[i + 2]) & 0xC0u) != 0x80u
                || (static_cast<unsigned char>(sv[i + 3]) & 0xC0u) != 0x80u) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

int testLocalizationAndUtf8Safety() {
    CHECK(resolveUiLanguage(UiLanguage::Auto, true) == ResolvedLang::Ja);
    CHECK(resolveUiLanguage(UiLanguage::Auto, false) == ResolvedLang::En);
    CHECK(resolveUiLanguage(UiLanguage::En, true) == ResolvedLang::En);
    CHECK(resolveUiLanguage(UiLanguage::Ja, false) == ResolvedLang::Ja);
    CHECK(parseUiLanguage("JA").value() == UiLanguage::Ja);
    CHECK(parseUiLanguage("en_US").value() == UiLanguage::En);
    CHECK(parseUiLanguage("auto").value() == UiLanguage::Auto);
    CHECK(!parseUiLanguage("fr").has_value());

    // UTF-8 boundary-safe truncation (never splits a 3-byte Japanese char).
    std::string jp = "あいうえおかきくけこ"; // 30 bytes
    for (size_t limit = 0; limit <= 32; ++limit) {
        std::string t = truncateUtf8Bytes(jp, limit);
        CHECK(t.size() <= limit);
        CHECK(isValidUtf8(t));
    }
    CHECK(truncateUtf8Bytes(jp, 5) == "あ");
    CHECK(truncateUtf8Bytes(jp, 6) == "あい");

    // Display width estimation distinguishes wide Japanese vs narrow ASCII & ignores § color codes.
    float wAscii = estimateDisplayWidth("Hello", 1.0f);
    float wJp    = estimateDisplayWidth("こんにちは", 1.0f);
    CHECK(wJp > wAscii * 1.4f);
    float wPlain = estimateDisplayWidth("Hub", 1.0f);
    float wCol   = estimateDisplayWidth("\xC2\xA7" "bHub", 1.0f);
    CHECK(std::abs(wPlain - wCol) < 0.01f);

    // fitTextToWidth always fits within pixel budget and keeps valid UTF-8.
    std::string fitted = fitTextToWidth("とても長い日本語のサーバー名テスト", 50.0f, 1.0f);
    CHECK(isValidUtf8(fitted));
    CHECK(estimateDisplayWidth(fitted, 1.0f) <= 50.5f);
    CHECK(fitted.ends_with(".."));

    return 0;
}

int testConfigJsoncRobustnessAndSampleSmoke() {
    RadialConfig def = makeDefaultConfig();
    CHECK(def.activationKey == kVkMenu);
    CHECK(def.editorKey == kVkF12);
    CHECK(def.language == UiLanguage::Auto);
    CHECK(def.activationMode == ActivationMode::Hold);
    CHECK(def.presets.size() >= 4);

    // Round-trip serialization & parsing.
    std::string  serialized = serializeConfigJson(def);
    RadialConfig reparsed;
    std::string  warn;
    CHECK(parseConfigJson(serialized, reparsed, &warn));
    CHECK(warn.empty());
    CHECK(reparsed.activationKey == def.activationKey);
    CHECK(reparsed.editorKey == def.editorKey);
    CHECK(reparsed.presets.size() == def.presets.size());
    CHECK(reparsed.presets[0].slots[1].children[2].value == "よろしく！");

    // UTF-8 BOM + JSONC comments + trailing commas + \uXXXX + surrogate pair + duplicate key (last-wins).
    std::string jsonc = "\xEF\xBB\xBF" R"({
        // line comment
        "activationKey": "R",
        "activationKey": "Tab",
        "activationMode": "toggle",
        "language": "ja",
        "invertWheelScroll": true,
        "flickSensitivity": 1.5,
        /* block comment */
        "presets": [
            {
                "id": "dup",
                "name": "\u3042\uD83D\uDE00",
                "slots": [
                    { "label": "OnlyOne", "command": "/hub", },
                ],
            },
            {
                "id": "dup",
                "name": "Second",
                "slots": [
                    { "label": "A", "chat": "hello" },
                    { "label": "B", "command": "/spawn" },
                ],
            },
        ],
    })";

    RadialConfig parsed;
    CHECK(parseConfigJson(jsonc, parsed));
    CHECK(parsed.activationKey == kVkTab); // last-wins!
    CHECK(parsed.activationMode == ActivationMode::Toggle);
    CHECK(parsed.language == UiLanguage::Ja);
    CHECK(parsed.invertWheelScroll == true);
    CHECK(parsed.presets.size() == 2);
    CHECK(parsed.presets[0].id != parsed.presets[1].id); // duplicate ID auto-deduped
    CHECK(parsed.presets[0].slots.size() >= static_cast<size_t>(kMinSlotsPerMenu)); // 1-slot padded to 2
    CHECK(isValidUtf8(parsed.presets[0].name));

    // Reject malformed inputs cleanly.
    RadialConfig bad;
    CHECK(!parseConfigJson(R"({ /* unterminated comment)", bad));
    CHECK(!parseConfigJson(R"({ "version": 0123 })", bad));          // leading zero
    CHECK(!parseConfigJson(R"({ "a": "\uD800" })", bad));            // lone high surrogate
    CHECK(!parseConfigJson(R"({ "a": "\uDC00" })", bad));            // lone low surrogate
    CHECK(!parseConfigJson(R"({ "a": "\u0000" })", bad));            // embedded NUL
    CHECK(!parseConfigJson(R"({ "a": "\q" })", bad));                // unknown escape
    CHECK(!parseConfigJson(R"({ "version": 1 } extra)", bad));       // trailing junk
    CHECK(!parseConfigJson(R"({ "version": 1e500 })", bad));         // non-finite overflow

    // Huge finite double safely clamped without UB, and NaN/Inf sanitized cleanly before serialize.
    RadialConfig clamped;
    CHECK(parseConfigJson(R"({ "version": 1e20, "globalCooldownMs": -1e20 })", clamped));
    CHECK(clamped.globalCooldownMs == 0);

    clamped.flickSensitivity = std::numeric_limits<float>::quiet_NaN();
    clamped.outerRadius      = std::numeric_limits<float>::infinity();
    std::string safeJson     = serializeConfigJson(clamped);
    RadialConfig afterNan;
    CHECK(parseConfigJson(safeJson, afterNan));
    CHECK(std::isfinite(afterNan.flickSensitivity));

    // Symmetric key matching for Alt, Shift, Ctrl + key name roundtrips.
    CHECK(keyMatchesBinding(kVkLMenu, kVkMenu));
    CHECK(keyMatchesBinding(kVkMenu, kVkLMenu));
    CHECK(keyMatchesBinding(kVkLShift, kVkShift));
    CHECK(keyMatchesBinding(kVkShift, kVkRShift));
    CHECK(keyMatchesBinding(kVkLControl, kVkControl));
    CHECK(keyMatchesBinding(kVkControl, kVkRControl));
    CHECK(parseKeyName("F12").value() == kVkF12);
    CHECK(parseKeyName("Insert").value() == kVkInsert);
    CHECK(parseKeyName("None").value() == 0);
    CHECK(!parseKeyName("F13").has_value());
    CHECK(parseKeyName("VK_65").value() == 65);
    CHECK(!parseKeyName("VK_65abc").has_value());
    CHECK(!parseKeyName("VK_99999999999999").has_value());

    // Smoke test: shipped config.sample.json parses cleanly with zero warnings!
    auto samplePath = std::filesystem::path("config.sample.json");
    if (std::filesystem::exists(samplePath)) {
        std::ifstream      in(samplePath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        RadialConfig sampleCfg;
        std::string  sampleWarn;
        CHECK(parseConfigJson(ss.str(), sampleCfg, &sampleWarn));
        CHECK(sampleWarn.empty());
        CHECK(sampleCfg.presets.size() == 4);
    }

    // Corrupt file backup test.
    auto corruptPath = std::filesystem::temp_directory_path() / "radial_corrupt_test.json";
    {
        std::ofstream out(corruptPath, std::ios::binary | std::ios::trunc);
        out << "{ broken json";
    }
    auto loadedCorrupt = loadConfigFile(corruptPath);
    CHECK(loadedCorrupt.existed && loadedCorrupt.corrupted);
    std::error_code ec;
    std::filesystem::remove(corruptPath, ec);
    for (auto const& ent : std::filesystem::directory_iterator(std::filesystem::temp_directory_path(), ec)) {
        if (ent.path().filename().string().starts_with("radial_corrupt_test.json.corrupt-")) {
            std::filesystem::remove(ent.path(), ec);
        }
    }

    return 0;
}

int testServerMatchingAndPlaceholders() {
    RadialConfig cfg = makeDefaultConfig();

    CHECK(wildcardMatchI("*.hivebedrock.network", "JP.HIVEBEDROCK.NETWORK"));
    CHECK(wildcardMatchI("server-?", "server-1"));
    CHECK(!wildcardMatchI("server-?", "server-12"));
    CHECK(isLoopbackHost("127.0.0.1"));
    CHECK(isLoopbackHost("::1"));
    CHECK(isLoopbackHost("localhost"));
    CHECK(isLoopbackHost("http://localhost:19132"));
    CHECK(isLoopbackHost("https://127.0.0.1:19132/path"));
    CHECK(isLoopbackHost("[::1]:19132"));
    CHECK(!isLoopbackHost("geo.hivebedrock.network"));

    // Endpoint normalization: strip URI scheme & embedded :port (prevents double-port labels & broken wildcards).
    {
        auto n1 = normalizeHostEndpoint("https://skynetwork-mcbe.net:19132");
        CHECK(n1.host == "skynetwork-mcbe.net" && n1.port == 19132);
        auto n2 = normalizeHostEndpoint("http://play.myserver.co.jp:19135/foo");
        CHECK(n2.host == "play.myserver.co.jp" && n2.port == 19135);
        auto n3 = normalizeHostEndpoint("[2001:db8::1]:19132");
        CHECK(n3.host == "2001:db8::1" && n3.port == 19132);
        auto n4 = normalizeHostEndpoint("2001:db8::1");
        CHECK(n4.host == "2001:db8::1" && n4.port == 0);

        ServerIdentity urlSrv;
        urlSrv.inWorld = true;
        urlSrv.isLocal = false;
        urlSrv.host    = "https://skynetwork-mcbe.net:19132";
        urlSrv.port    = 19132;
        CHECK(urlSrv.displayLabel() == "skynetwork-mcbe.net:19132");
        CHECK(suggestHostPattern(urlSrv) == "skynetwork-mcbe.net");

        ServerIdentity urlSub = urlSrv;
        urlSub.host           = "https://play.myserver.co.jp:19135";
        urlSub.port           = 0;
        CHECK(urlSub.displayLabel() == "play.myserver.co.jp:19135");
        CHECK(suggestHostPattern(urlSub) == "*.myserver.co.jp");
    }

    // Lexicographic (priority, specificity) regression tests:
    // 1) Negative priority still matches (never rejected as score < 0!).
    ServerIdentity srv;
    srv.inWorld    = true;
    srv.isLocal    = false;
    srv.host       = "play.example.net";
    srv.port       = 19132;
    srv.serverName = "Example";

    ServerRule negPrio{"*.example.net", 0, "", "", ConnectionScope::External, -5};
    auto       negScore = evaluateServerRule(negPrio, srv);
    CHECK(negScore.matched);
    CHECK(negScore.priority == -5);

    // 2) Higher priority ALWAYS beats higher specificity (no priority*100 overflow!).
    ServerRule lowPrioHighSpec{"play.example.net", 19132, "Example", "", ConnectionScope::External, 10};
    ServerRule highPrioLowSpec{"*.example.net", 0, "", "", ConnectionScope::Any, 11};
    auto       scLow  = evaluateServerRule(lowPrioHighSpec, srv);
    auto       scHigh = evaluateServerRule(highPrioLowSpec, srv);
    CHECK(scLow.matched && scHigh.matched);
    CHECK(scHigh.betterThan(scLow));

    // Empty rule with scope=Any is rejected.
    ServerRule emptyRule;
    CHECK(!evaluateServerRule(emptyRule, srv).matched);

    // ccTLD protection in suggestHostPattern.
    ServerIdentity jp3;
    jp3.inWorld = true;
    jp3.isLocal = false;
    jp3.host    = "myserver.co.jp";
    CHECK(suggestHostPattern(jp3) == "myserver.co.jp"); // NOT "*.co.jp"!

    ServerIdentity jp4 = jp3;
    jp4.host           = "play.myserver.co.jp";
    CHECK(suggestHostPattern(jp4) == "*.myserver.co.jp");

    ServerIdentity realm;
    realm.inWorld = true;
    realm.isLocal = false;
    realm.isRealm = true;
    realm.ip      = "20.1.2.3";
    CHECK(suggestHostPattern(realm).empty());

    // Placeholders expansion.
    PlaceholderContext pctx{"Steve", 12, 64, -30, "The Hive", "Lobby"};
    CHECK(expandPlaceholders("Hi {player} at {x} {y} {z} on {server}/{world}", pctx)
          == "Hi Steve at 12 64 -30 on The Hive/Lobby");
    CHECK(expandPlaceholders("No braces here", pctx) == "No braces here");
    CHECK(expandPlaceholders("Unknown {foo} stays", pctx) == "Unknown {foo} stays");

    return 0;
}

int testRadialControllerAdvanced() {
    RadialConfig     cfg = makeDefaultConfig();
    cfg.globalCooldownMs = 500;
    cfg.submenuHoverMs   = 300;

    RadialController ctrl;
    ctrl.setConfig(cfg, 1000);

    // 1) Auto-switch on server join + sub-server transfer toast suppression.
    ServerIdentity hiveLobby;
    hiveLobby.inWorld    = true;
    hiveLobby.isLocal    = false;
    hiveLobby.host       = "geo.hivebedrock.network";
    hiveLobby.port       = 19132;
    CHECK(ctrl.updateServerIdentity(hiveLobby, cfg, 1100, ResolvedLang::Ja));
    CHECK(ctrl.activePresetIndex() == findPresetIndex(cfg, "hive"));

    // Transfer to another Hive game server port -> same auto preset, no change!
    ServerIdentity hiveGame = hiveLobby;
    hiveGame.port           = 19135;
    CHECK(!ctrl.updateServerIdentity(hiveGame, cfg, 1500, ResolvedLang::Ja));

    // 2) Independent per-slot cooldown vs global cooldown!
    // In Hive preset: slot 0 = Hub (uses global 500ms), slot 3 = GG+Hub (per-slot 1500ms).
    ctrl.onActivationDown(cfg, 2000);
    ctrl.onMouseDelta(0.0f, -90.0f, cfg, 2010);
    CHECK(ctrl.hoveredIndex() == 0);
    CHECK(ctrl.onActivationUp(cfg, 2020));
    CHECK(ctrl.pollDueActions(2020).size() == 1);

    // 100ms later: slot 0 (global CD) is blocked, BUT slot 3 (independent per-slot CD, never run yet) SUCCEEDS!
    ctrl.onActivationDown(cfg, 2100);
    ctrl.onMouseDelta(80.0f, 80.0f, cfg, 2110);
    CHECK(ctrl.hoveredIndex() == 3);
    CHECK(ctrl.onActivationUp(cfg, 2120));
    auto mStep1 = ctrl.pollDueActions(2120);
    CHECK(mStep1.size() == 1 && mStep1[0].value == "gg");
    CHECK(ctrl.pollDueActions(2300).empty());
    auto mStep2 = ctrl.pollDueActions(2370);
    CHECK(mStep2.size() == 1 && mStep2[0].value == "/hub");

    // 200ms later (at 2320ms): slot 3's own 1500ms CD blocks slot 3!
    ctrl.onActivationDown(cfg, 2320);
    ctrl.onMouseDelta(80.0f, 80.0f, cfg, 2325);
    CHECK(!ctrl.onActivationUp(cfg, 2330));

    // 3) Toggle mode test.
    cfg.activationMode = ActivationMode::Toggle;
    ctrl.onActivationDown(cfg, 5000);
    CHECK(ctrl.isWheelOpen());
    // Releasing key in Toggle mode keeps wheel open!
    CHECK(!ctrl.onActivationUp(cfg, 5050));
    CHECK(ctrl.isWheelOpen());
    // Flick up & left-click executes slot 0 and closes wheel.
    ctrl.onMouseDelta(0.0f, -90.0f, cfg, 5060);
    CHECK(ctrl.onLeftClick(cfg, 5070));
    CHECK(!ctrl.isWheelOpen());
    CHECK(ctrl.pollDueActions(5070).size() == 1);

    // 4) Submenu hover + inertia grace period test.
    cfg.activationMode = ActivationMode::Hold;
    ctrl.onActivationDown(cfg, 6000);
    ctrl.onMouseDelta(90.0f, 0.0f, cfg, 6010); // slot 2 = Quick Chat submenu
    CHECK(ctrl.hoveredIndex() == 2);
    ctrl.update(cfg, 6320);
    CHECK(ctrl.buildSnapshot(cfg, 6320).inSubmenu);
    // Motion during 120ms inertia grace is ignored so it doesn't accidentally jump to a child!
    ctrl.onMouseDelta(0.0f, -90.0f, cfg, 6350);
    CHECK(ctrl.hoveredIndex() == -1);
    // Motion after grace period selects child slot 0 ("GG").
    ctrl.onMouseDelta(0.0f, -90.0f, cfg, 6500);
    CHECK(ctrl.hoveredIndex() == 0);
    CHECK(ctrl.onActivationUp(cfg, 6520));
    auto subAct = ctrl.pollDueActions(6520);
    CHECK(subAct.size() == 1 && subAct[0].value == "gg");

    // 5) holdDelayMs arming does NOT capture camera while arming.
    cfg.holdDelayMs = 150;
    ctrl.onActivationDown(cfg, 8000);
    CHECK(ctrl.isArming());
    CHECK(!ctrl.isCapturingInput()); // camera remains unlocked during arming!
    ctrl.update(cfg, 8160);
    CHECK(ctrl.isWheelOpen());
    CHECK(ctrl.isCapturingInput());
    ctrl.cancelWheel();

    // 6) Invert wheel scroll.
    cfg.holdDelayMs       = 0;
    cfg.invertWheelScroll = true;
    ctrl.onActivationDown(cfg, 9000);
    int beforeP = ctrl.activePresetIndex();
    CHECK(ctrl.onMouseWheel(1, cfg, 9010));
    CHECK(ctrl.activePresetIndex() == (beforeP + 1) % static_cast<int>(cfg.presets.size()));
    ctrl.cancelWheel();

    return 0;
}

int testGeometryRasterizer() {
    std::vector<geom::RectF> rects;
    auto                     a = geom::computeSectorAngles(0, 8, 0.04f);
    geom::rasterizeAnnularSector(200.0f, 120.0f, 32.0f, 92.0f, a.startAngle, a.endAngle, 1.0f, rects);
    CHECK(!rects.empty());
    for (auto const& r : rects) {
        CHECK(r.x1 > r.x0);
        CHECK(r.y1 > r.y0);
    }

    rects.clear();
    geom::rasterizeDisc(100.0f, 100.0f, 20.0f, 1.0f, rects);
    CHECK(rects.size() >= 35 && rects.size() <= 45);

    rects.clear();
    geom::rasterizeLine(0.0f, 0.0f, 50.0f, 35.0f, 1.5f, rects);
    CHECK(!rects.empty());
    return 0;
}

int testEditorModelFullWorkflow() {
    RadialConfig   cfg = makeDefaultConfig();
    ServerIdentity srv;
    srv.inWorld    = true;
    srv.isLocal    = false;
    srv.host       = "play.myserver.co.jp";
    srv.port       = 19135;
    srv.serverName = "MyServer";

    editor::EditorModel ed;
    ed.beginSession(cfg, srv, 0, true);
    CHECK(!ed.isDirty());
    CHECK(ed.resolvedLang() == ResolvedLang::Ja);
    CHECK(ed.activeTab() == editor::EditorTab::Wheel);

    // Responsive check even on small screen (360x220): panel stays inside screen!
    auto smallL = ed.buildLayout(360.0f, 220.0f);
    CHECK(smallL.panel.x0 >= 0.0f && smallL.panel.x1 <= 360.0f);
    CHECK(smallL.panel.y0 >= 0.0f && smallL.panel.y1 <= 220.0f);

    // Click sector 2 on circular wheel preview.
    auto layout = ed.buildLayout(540.0f, 304.0f);
    auto secPt  = geom::sectorLabelCenter(
        layout.wheelPreview.cx,
        layout.wheelPreview.cy,
        (layout.wheelPreview.innerR + layout.wheelPreview.outerR) * 0.5f,
        2,
        static_cast<int>(layout.wheelPreview.slots.size())
    );
    ed.handleClick(secPt.x, secPt.y, 540.0f, 304.0f);
    CHECK(ed.selectedSlot() == 2);

    // Language toggle [JA/EN].
    for (auto const& b : layout.buttons) {
        if (b.id == "lang_toggle") {
            ed.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
            break;
        }
    }
    CHECK(ed.resolvedLang() == ResolvedLang::En);
    CHECK(ed.isDirty());

    // Esc while dirty triggers unsaved-changes confirmation instead of silently losing work!
    CHECK(ed.handleKey(kVkEscape, false, false) == editor::EditorCommand::None);
    CHECK(ed.buildLayout(540.0f, 304.0f).confirmingClose);
    CHECK(ed.handleKey(kVkEscape, false, false) == editor::EditorCommand::None); // cancels confirm banner
    CHECK(!ed.buildLayout(540.0f, 304.0f).confirmingClose);

    // Undo restores Japanese language!
    CHECK(ed.canUndo());
    CHECK(ed.undo());
    CHECK(ed.resolvedLang() == ResolvedLang::Ja);

    // Switch to Servers tab: create preset for current server (ccTLD & non-default port preserved).
    for (auto const& b : layout.buttons) {
        if (b.id == "tab:servers") {
            ed.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
            break;
        }
    }
    CHECK(ed.activeTab() == editor::EditorTab::Servers);
    size_t beforeP = ed.draft().presets.size();
    CHECK(ed.createPresetForCurrentServer());
    CHECK(ed.draft().presets.size() == beforeP + 1);
    CHECK(ed.currentPreset()->serverRules.front().hostPattern == "*.myserver.co.jp");
    CHECK(ed.currentPreset()->serverRules.front().port == 19135);
    CHECK(ed.buildLayout(540.0f, 304.0f).presetMatchesCurrent);

    // Switch to Settings tab: click & drag slider!
    for (auto const& b : layout.buttons) {
        if (b.id == "tab:settings") {
            ed.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
            break;
        }
    }
    CHECK(ed.activeTab() == editor::EditorTab::Settings);
    auto setL = ed.buildLayout(540.0f, 304.0f);
    for (auto const& sl : setL.sliders) {
        if (sl.id == "sens") {
            ed.handleClick(sl.rect.x0 + sl.rect.width() * 0.25f, sl.rect.y0 + 3.0f, 540.0f, 304.0f);
            CHECK(ed.isDraggingSlider());
            ed.handleDrag(sl.rect.x0 + sl.rect.width() * 0.75f, sl.rect.y0 + 3.0f, 540.0f, 304.0f);
            ed.endDrag();
            break;
        }
    }
    CHECK(ed.draft().flickSensitivity > 2.5f);

    // Switch back to Wheel tab: test cursor movement, Delete (1 char!), ToUnicode UTF-8 input, Ctrl+A/C/V.
    for (auto const& b : setL.buttons) {
        if (b.id == "tab:wheel") {
            ed.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
            break;
        }
    }
    auto wL = ed.buildLayout(540.0f, 304.0f);
    for (auto const& tb : wL.textBoxes) {
        if (tb.field == editor::TextFieldId::SlotValue) {
            ed.handleClick(tb.rect.x0 + 4.0f, tb.rect.y0 + 4.0f, 540.0f, 304.0f);
            break;
        }
    }
    ed.handleKey('A', false, true); // Ctrl+A
    ed.handleKey('V', false, true, "あいうえ"); // replaces with Japanese
    CHECK(ed.currentSlot()->value == "あいうえ");

    std::string copied;
    ed.handleKey('C', false, true, {}, {}, &copied); // Ctrl+C
    CHECK(copied == "あいうえ");

    ed.handleKey(kVkHome, false, false);
    ed.handleKey(kVkDelete, false, false); // deletes ONLY "あ", not entire string!
    CHECK(ed.currentSlot()->value == "いうえ");

    ed.handleKey(kVkRight, false, false);
    ed.handleKey(0xBA, false, false, {}, ":"); // ToUnicodeEx JIS symbol input!
    CHECK(ed.currentSlot()->value == "い:うえ");

    // Caret text viewport slicing (ASCII + multibyte UTF-8 + scrolling).
    {
        auto fixedMeas = [](std::string_view sv) -> float {
            return estimateDisplayWidth(sv, 1.0f);
        };
        auto sEmpty = sliceTextForCaret("", 0, 80.0f, 1.0f, fixedMeas);
        CHECK(sEmpty.visibleText.empty() && sEmpty.caretOffsetPx == 0.0f);

        auto sFit = sliceTextForCaret("skynetwork", 10, 120.0f, 1.0f, fixedMeas);
        CHECK(sFit.visibleText == "skynetwork");
        CHECK(sFit.caretOffsetPx > 10.0f && sFit.caretOffsetPx < 120.0f);

        auto sMid = sliceTextForCaret("skynetwork", 3, 120.0f, 1.0f, fixedMeas);
        CHECK(sMid.visibleText == "skynetwork");
        CHECK(sMid.caretOffsetPx > 0.0f && sMid.caretOffsetPx < sFit.caretOffsetPx);

        // Long string scrolls horizontally to keep caret in view without splitting UTF-8.
        std::string longJp = "あいうえおかきくけこさしすせそ";
        auto sScroll = sliceTextForCaret(longJp, longJp.size(), 40.0f, 1.0f, fixedMeas);
        CHECK(!sScroll.visibleText.empty());
        CHECK(sScroll.visibleText.size() < longJp.size());
        CHECK(sScroll.visibleText.ends_with("そ"));
        CHECK(sScroll.caretOffsetPx <= 40.0f);

        // Click-to-caret placement (start, middle, end, multibyte UTF-8 boundary).
        CHECK(caretByteFromClickOffset("skynetwork", 0, 0.0f, 120.0f, 1.0f, fixedMeas) == 0);
        size_t midClick = caretByteFromClickOffset("skynetwork", 0, sMid.caretOffsetPx, 120.0f, 1.0f, fixedMeas);
        CHECK(midClick == 3);
        CHECK(caretByteFromClickOffset("skynetwork", 0, 200.0f, 120.0f, 1.0f, fixedMeas) == 10);
        size_t jpClick = caretByteFromClickOffset("あいう", 0, estimateDisplayWidth("あ", 1.0f), 120.0f, 1.0f, fixedMeas);
        CHECK(jpClick == std::string("あ").size());
    }

    // Tab / Shift+Tab field cycling, double-click select-all, hover hints, and F12 close flow.
    {
        auto wLayout = ed.buildLayout(540.0f, 304.0f);
        CHECK(ed.focusedField() == editor::TextFieldId::SlotValue);
        ed.handleKey(kVkTab, false, false);
        CHECK(ed.focusedField() == editor::TextFieldId::SlotSubtitle);
        ed.handleKey(kVkTab, true, false);
        CHECK(ed.focusedField() == editor::TextFieldId::SlotValue);

        // Hover hint resolves cleanly for SlotValue textbox and Save button.
        for (auto const& tb : wLayout.textBoxes) {
            if (tb.field == editor::TextFieldId::SlotValue) {
                CHECK(!editor::EditorModel::hoverHintAt(wLayout, tb.rect.x0 + 4.0f, tb.rect.y0 + 4.0f).empty());
                // Double-click selects all.
                ed.handleClick(tb.rect.x0 + 4.0f, tb.rect.y0 + 4.0f, 540.0f, 304.0f, {}, true);
                auto afterDbl = ed.buildLayout(540.0f, 304.0f);
                for (auto const& tb2 : afterDbl.textBoxes) {
                    if (tb2.field == editor::TextFieldId::SlotValue) CHECK(tb2.selectedAll);
                }
                break;
            }
        }
        // Priority label exists on Servers tab.
        for (auto const& b : wLayout.buttons) {
            if (b.id == "tab:servers") {
                ed.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
                break;
            }
        }
        auto sLayout = ed.buildLayout(540.0f, 304.0f);
        CHECK(!sLayout.labels.empty());

        // Regression: fill_wname on an external server preserves External scope (never overwrites to Local).
        ServerIdentity extWithWorld = srv;
        extWithWorld.worldName      = "LobbyWorld";
        editor::EditorModel edExt;
        edExt.beginSession(cfg, extWithWorld, 0, false);
        for (auto const& b : edExt.buildLayout(540.0f, 304.0f).buttons) {
            if (b.id == "tab:servers") {
                edExt.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
                break;
            }
        }
        for (auto const& b : edExt.buildLayout(540.0f, 304.0f).buttons) {
            if (b.id == "fill_wname") {
                edExt.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
                break;
            }
        }
        CHECK(edExt.currentRule() != nullptr);
        CHECK(edExt.currentRule()->scope == ConnectionScope::External);
        CHECK(edExt.currentRule()->worldNamePattern == "*LobbyWorld*");

        // Regression: pattern field accepts up to 128 bytes (matching sanitizeConfig).
        for (auto const& tb : edExt.buildLayout(540.0f, 304.0f).textBoxes) {
            if (tb.field == editor::TextFieldId::HostPattern) {
                edExt.handleClick(tb.rect.x0 + 3.0f, tb.rect.y0 + 3.0f, 540.0f, 304.0f);
                break;
            }
        }
        std::string longPat(140, 'a');
        edExt.insertText(longPat);
        CHECK(edExt.currentRule()->hostPattern.size() == 128);

        // Regression: tog_page cycles strictly within actual page count (maxTogglePage + 1) and sl_next clamps.
        for (auto const& b : edExt.buildLayout(540.0f, 304.0f).buttons) {
            if (b.id == "tab:settings") {
                edExt.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
                break;
            }
        }
        auto setLay1 = edExt.buildLayout(540.0f, 304.0f);
        int  togPages = setLay1.maxTogglePage + 1;
        CHECK(togPages >= 2);
        std::string firstTogLabel;
        for (auto const& b : setLay1.buttons) {
            if (b.id == "tog_page") firstTogLabel = b.label;
        }
        for (int i = 0; i < togPages; ++i) {
            auto curL = edExt.buildLayout(540.0f, 304.0f);
            for (auto const& b : curL.buttons) {
                if (b.id == "tog_page") {
                    edExt.handleClick(b.rect.x0 + 3.0f, b.rect.y0 + 3.0f, 540.0f, 304.0f);
                    break;
                }
            }
        }
        std::string afterCycleTogLabel;
        for (auto const& b : edExt.buildLayout(540.0f, 304.0f).buttons) {
            if (b.id == "tog_page") afterCycleTogLabel = b.label;
        }
        CHECK(afterCycleTogLabel == firstTogLabel);
    }

    // Save & load atomic roundtrip.
    auto tmpPath = std::filesystem::temp_directory_path() / "radial_command_mod_v1_test_config.json";
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
    CHECK(saveConfigFile(tmpPath, ed.draft()));
    auto loaded = loadConfigFile(tmpPath);
    CHECK(loaded.existed && !loaded.corrupted);
    CHECK(loaded.config.presets.size() == ed.draft().presets.size());
    std::filesystem::remove(tmpPath, ec);

    return 0;
}

} // namespace

int main() {
    if (testLocalizationAndUtf8Safety() != 0) return 1;
    if (testConfigJsoncRobustnessAndSampleSmoke() != 0) return 1;
    if (testServerMatchingAndPlaceholders() != 0) return 1;
    if (testRadialControllerAdvanced() != 0) return 1;
    if (testGeometryRasterizer() != 0) return 1;
    if (testEditorModelFullWorkflow() != 0) return 1;

    std::cout << "[PASS] All " << gTestsPassed << " RadialCommandMod unit checks passed!\n";
    return 0;
}
