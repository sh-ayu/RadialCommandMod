#include "core/Localization.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace radial {
namespace {

std::string toLowerTrim(std::string_view sv) {
    size_t b = 0, e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(sv[i]))));
    return out;
}

size_t utf8CharLen(unsigned char lead) noexcept {
    if ((lead & 0x80u) == 0x00u) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

uint32_t decodeUtf8(std::string_view sv, size_t& i) noexcept {
    unsigned char b0 = static_cast<unsigned char>(sv[i]);
    if (b0 < 0x80u) {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && i + 1 < sv.size()) {
        uint32_t cp = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(sv[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0u) == 0xE0u && i + 2 < sv.size()) {
        uint32_t cp = ((b0 & 0x0Fu) << 12)
                    | ((static_cast<unsigned char>(sv[i + 1]) & 0x3Fu) << 6)
                    | (static_cast<unsigned char>(sv[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((b0 & 0xF8u) == 0xF0u && i + 3 < sv.size()) {
        uint32_t cp = ((b0 & 0x07u) << 18)
                    | ((static_cast<unsigned char>(sv[i + 1]) & 0x3Fu) << 12)
                    | ((static_cast<unsigned char>(sv[i + 2]) & 0x3Fu) << 6)
                    | (static_cast<unsigned char>(sv[i + 3]) & 0x3Fu);
        i += 4;
        return cp;
    }
    ++i;
    return 0xFFFDu;
}

bool isWideCodepoint(uint32_t cp) noexcept {
    return (cp >= 0x1100u && cp <= 0x115Fu)
        || (cp >= 0x2E80u && cp <= 0x9FFFu)
        || (cp >= 0xAC00u && cp <= 0xD7A3u)
        || (cp >= 0xF900u && cp <= 0xFAFFu)
        || (cp >= 0xFE10u && cp <= 0xFE6Fu)
        || (cp >= 0xFF00u && cp <= 0xFF60u)
        || (cp >= 0xFFE0u && cp <= 0xFFE6u)
        || (cp >= 0x1F300u && cp <= 0x1FAFFu)
        || (cp >= 0x20000u && cp <= 0x3FFFDu);
}

struct StrPair {
    std::string_view key;
    char const*      ja;
    char const*      en;
};

constexpr StrPair kDict[] = {
    // Wheel HUD
    {"hud.cancel",              "キャンセル",                "CANCEL"},
    {"hud.back",                "戻る",                      "BACK"},
    {"hud.hint.flick_root",     "振って離す:実行 | 右クリック:閉じる", "Flick+Release: Run | RClick: Cancel"},
    {"hud.hint.flick_sub",      "振って離す:実行 | 右クリック:戻る",   "Flick+Release: Run | RClick: Back"},
    {"hud.hint.toggle_root",    "振って左クリック:実行 | 右:閉じる",   "Flick+LClick: Run | RClick: Cancel"},
    {"hud.hint.toggle_sub",     "振って左クリック:実行 | 右:戻る",     "Flick+LClick: Run | RClick: Back"},
    {"hud.hint.preset",         " | ホイール/1-9:切替",       " | Wheel/1-9: Preset"},
    {"hud.submenu_tag",         "サブメニュー",              "Submenu"},
    {"hud.macro_tag",           "マクロ",                    "Macro"},
    {"hud.steps",               "ステップ",                  "steps"},
    {"hud.toast.preset",        "プリセット: ",              "Preset: "},
    {"hud.toast.auto",          " (自動)",                   " (Auto)"},
    {"hud.toast.cooldown",      "クールダウン中",            "Cooldown"},

    // Editor Header & Tabs
    {"ed.title",                "RadialCommandMod 設定",     "RadialCommandMod Editor"},
    {"ed.connected",            "接続先: ",                  "Server: "},
    {"ed.save",                 "保存",                      "Save"},
    {"ed.save_dirty",           "保存 *",                    "Save *"},
    {"ed.reload",               "再読込",                    "Reload"},
    {"ed.undo",                 "戻す",                      "Undo"},
    {"ed.close",                "閉じる",                    "Close"},
    {"ed.tab.wheel",            "1. ホイール",               "1. Wheel"},
    {"ed.tab.servers",          "2. サーバー",               "2. Servers"},
    {"ed.tab.settings",         "3. 設定",                   "3. Settings"},

    // Unsaved confirmation banner
    {"ed.unsaved.prompt",       "未保存の変更があります:",    "Unsaved changes:"},
    {"ed.unsaved.save_close",   "保存して閉じる",            "Save & Close"},
    {"ed.unsaved.discard",      "破棄して閉じる",            "Discard"},
    {"ed.unsaved.cancel",       "編集に戻る",                "Stay"},

    // Reset confirmation
    {"ed.reset.btn",            "初期設定に戻す",            "Reset Defaults"},
    {"ed.reset.confirm",        "本当に初期化？(再クリック)", "Confirm Reset?"},

    // Tab 1: Wheel
    {"ed.preset.new",           "+新規",                     "+New"},
    {"ed.preset.dup",           "複製",                      "Dup"},
    {"ed.preset.del",           "削除",                      "Del"},
    {"ed.submenu.back",         "◀ 親へ戻る",                "< Parent"},
    {"ed.slot.add",             "+追加",                     "+Add"},
    {"ed.slot.dup",             "複製",                      "Dup"},
    {"ed.slot.prev",            "◀移動",                     "<Move"},
    {"ed.slot.next",            "移動▶",                     "Move>"},
    {"ed.slot.del",             "削除",                      "Del"},
    {"ed.slot.test",            "テスト",                    "Test"},
    {"ed.slot.header",          "スロット #",                "Slot #"},
    {"ed.slot.label",           "表示名",                    "Label"},
    {"ed.slot.subtitle",        "補足",                      "Subtitle"},
    {"ed.slot.cmd_cap",         "コマンド",                  "Command"},
    {"ed.slot.chat_cap",        "メッセージ",                "Message"},
    {"ed.slot.cli_cap",         "ローカルコマンド",          "Client Command"},
    {"ed.slot.sub_open",        "サブメニューを編集 ▶",      "Edit Submenu >"},
    {"ed.slot.sub_max",         "最大階層 (4段)",            "Max Depth (4)"},
    {"ed.type.cmd",             "コマンド",                  "Cmd"},
    {"ed.type.chat",            "チャット",                  "Chat"},
    {"ed.type.cli",             "ローカル",                  "Client"},
    {"ed.type.macro",           "マクロ",                    "Macro"},
    {"ed.type.sub",             "サブ",                      "Sub"},
    {"ed.color.hdr",            "色",                        "Color"},
    {"ed.color.auto",           "自動",                      "Auto"},
    {"ed.cd.hdr",               "クールダウン",              "Cooldown"},
    {"ed.cd.global",            "共通設定",                  "Global"},
    {"ed.macro.add",            "+手順",                     "+Step"},
    {"ed.macro.type",           "種別切替",                  "Type"},
    {"ed.macro.up",             "▲上へ",                     "Up"},
    {"ed.macro.del",            "削除",                      "Del"},
    {"ed.macro.step_cap",       "実行内容",                  "Step Action"},
    {"ed.macro.delay_hdr",      "待機時間",                  "Delay"},
    {"ed.wheel.divisions",      "分割",                      "slots"},

    // Tab 2: Servers
    {"ed.srv.list",             "プリセット一覧",            "Presets"},
    {"ed.srv.rules",            "自動切替ルール",            "Auto-Switch Rules"},
    {"ed.srv.create_cur",       "+ 今の接続先用を作成",      "+ Create for Current"},
    {"ed.srv.match",            "一致",                      "MATCH"},
    {"ed.srv.nomatch",          "不一致",                    "NO MATCH"},
    {"ed.srv.preset_name",      "プリセット名",              "Preset Name"},
    {"ed.srv.rule_none",        "ルールなし",                "No Rules"},
    {"ed.srv.rule_prefix",      "ルール ",                   "Rule "},
    {"ed.srv.scope_any",        "すべて",                    "Any"},
    {"ed.srv.scope_ext",        "マルチ",                    "Server"},
    {"ed.srv.scope_loc",        "ローカル",                  "Local"},
    {"ed.srv.host_pat",         "ホスト / IP",               "Host / IP"},
    {"ed.srv.sname_pat",        "サーバー名",                "Server Name"},
    {"ed.srv.wname_pat",        "ワールド名",                "World Name"},
    {"ed.srv.port",             "ポート",                    "Port"},
    {"ed.srv.fill",             "取得",                      "Fill"},
    {"ed.srv.rule_add",         "+追加",                     "+Rule"},
    {"ed.srv.rule_del",         "削除",                      "Del"},
    {"ed.srv.prio",             "優先度",                    "Priority"},

    // Tab 3: Settings
    {"ed.set.keys_hdr",         "キー・操作モード",          "Keys & Behavior"},
    {"ed.set.tune_hdr",         "見た目・感度",              "Appearance & Tuning"},
    {"ed.set.hold_key",         "起動キー: ",                "Wheel Key: "},
    {"ed.set.editor_key",       "エディタキー: ",            "Editor Key: "},
    {"ed.set.press_key",        ">> キーを押してください <<", ">> Press any key <<"},
    {"ed.set.mode_hold",        "長押し",                    "Hold"},
    {"ed.set.mode_toggle",      "トグル",                    "Toggle"},
    {"ed.set.en_mod",           "MOD 有効",                  "Enable Mod"},
    {"ed.set.en_release",       "離して実行",                "Run on Release"},
    {"ed.set.en_click",         "左クリック実行",            "Run on Left Click"},
    {"ed.set.en_auto",          "サーバー自動切替",          "Auto-Switch Preset"},
    {"ed.set.en_toast",         "切替トースト通知",          "Preset Switch Toast"},
    {"ed.set.en_hints",         "1-9 切替ヒント表示",        "Show 1-9 Hints"},
    {"ed.set.en_invwheel",      "ホイール切替方向を反転",    "Invert Wheel Scroll"},
    {"ed.set.page_prev",        "◀ 前へ",                    "< Prev"},
    {"ed.set.page_next",        "次へ ▶",                    "Next >"},
    {"ed.set.tog_next",         "次ページ ",                 "More "},
    {"ed.set.sl.sens",          "フリック感度",              "Flick Sensitivity"},
    {"ed.set.sl.outer",         "ホイール外半径",            "Outer Radius"},
    {"ed.set.sl.inner",         "ホイール内半径",            "Inner Radius"},
    {"ed.set.sl.dead",          "デッドゾーン",              "Deadzone Radius"},
    {"ed.set.sl.opacity",       "背景不透明度",              "Background Opacity"},
    {"ed.set.sl.cd",            "共通クールダウン",          "Global Cooldown"},
    {"ed.set.sl.sub",           "サブ自動展開時間",          "Submenu Hover Delay"},
    {"ed.set.sl.hold",          "長押し開始遅延",            "Hold Open Delay"},
    {"ed.set.sl.font",          "文字サイズ",                "Font Scale"},

    // Hover hints (shown cleanly in footer bar on mouse hover)
    {"hint.save",               "変更を config.json に保存 (Ctrl+S)", "Save changes to config.json (Ctrl+S)"},
    {"hint.undo",               "直前の変更を1つ元に戻す (Ctrl+Z)", "Undo the last change (Ctrl+Z)"},
    {"hint.reload",             "config.json をディスクから再読み込み", "Reload config.json from disk"},
    {"hint.lang",               "UIの表示言語を 日本語 / English で切替", "Switch UI language between Japanese and English"},
    {"hint.close",              "エディタを閉じる (Esc / F12)", "Close editor (Esc / F12)"},
    {"hint.wheel_preview",      "クリックでスロット選択（サブメニューは再クリックで展開）", "Click a slice to edit (click Submenu again to open)"},
    {"hint.slot_test",          "このスロットを今すぐゲーム内でテスト実行", "Run this slot immediately to test it"},
    {"hint.slot_label",         "ホイール上に表示される名前（Tabで次の欄へ）", "Name shown on the radial slice (Tab for next field)"},
    {"hint.slot_subtitle",      "ホイール中央に表示される補足メモ（空欄可）", "Optional subtitle shown in the wheel center"},
    {"hint.slot_cmd",           "サーバーへ送信するコマンド（{player} {x} {y} {z} {server} {world} 対応）", "Server command ({player} {x} {y} {z} {server} {world} supported)"},
    {"hint.slot_chat",          "チャットへ送信する文章（Ctrl+V 貼り付け・{x} {y} {z} 対応）", "Chat message (Ctrl+V paste & {x} {y} {z} supported)"},
    {"hint.slot_cli",           "クライアント側コマンドを実行（/rc 等）", "Run a client-side command (such as /rc)"},
    {"hint.type_cmd",           "サーバーコマンドを実行するスロット", "Execute a server slash command"},
    {"hint.type_chat",          "チャットメッセージを送信するスロット", "Send a chat message"},
    {"hint.type_cli",           "クライアント側コマンドを実行するスロット", "Execute a client-side command"},
    {"hint.type_macro",         "複数のコマンド/チャットを時間差で連続実行", "Run multiple commands/chats in sequence with delays"},
    {"hint.type_sub",           "下層ホイール（サブメニュー）を開くスロット", "Open a nested submenu wheel"},
    {"hint.slot_move",          "選択中スロットのホイール上の位置を入れ替え", "Reorder the selected slot on the wheel"},
    {"hint.color",              "スロットの強調色（自動＝種別に応じた色）", "Slice accent color (Auto = color by action type)"},
    {"hint.slot_cd",            "このスロット専用の連打防止時間（中央クリックで共通設定に戻す）", "Per-slot cooldown (click center to reset to Global)"},
    {"hint.macro_delay",        "前の手順からこの手順を実行するまでの待機時間 (ms)", "Delay before running this step (ms)"},
    {"hint.srv_create",         "今接続しているサーバー/ワールドに自動切替する新プリセットを作成", "Create a new preset matched to the current server/world"},
    {"hint.srv_rule_nav",       "このプリセットの自動切替ルールを切替・追加・削除（いずれか一致で適用）", "Switch/add/delete auto-switch rules (any matching rule activates)"},
    {"hint.srv_scope",          "適用対象の接続種別（すべて / マルチサーバー / ローカルワールド）", "Connection scope (Any / External Server / Local World)"},
    {"hint.srv_host",           "接続先アドレス条件（* と ? のワイルドカード対応・空欄で無視）", "Host/IP pattern (* and ? wildcards; empty = ignore)"},
    {"hint.srv_sname",          "サーバー名条件（* と ? のワイルドカード対応・空欄で無視）", "Server name pattern (* and ? wildcards; empty = ignore)"},
    {"hint.srv_wname",          "ワールド名条件（* と ? のワイルドカード対応・空欄で無視）", "World name pattern (* and ? wildcards; empty = ignore)"},
    {"hint.srv_fill",           "現在接続中のサーバー/ワールドから自動入力", "Auto-fill from current server/world"},
    {"hint.srv_port",           "対象ポート番号（0 で全ポートに一致）", "Target port number (0 matches any port)"},
    {"hint.srv_prio",           "複数プリセットが一致した時の優先順位（高い方が優先）", "Rule priority when multiple presets match (higher wins)"},
    {"hint.set_actkey",         "クリック後、ホイールを開くキーまたはマウスサイドボタンを押して設定", "Click then press any key or mouse side button"},
    {"hint.set_edkey",          "クリック後、このエディタを開くキーを押して設定", "Click then press the hotkey to open this editor"},
    {"hint.set_mode_hold",      "キー長押し中だけホイールを開き、離して実行", "Hold key to open wheel, release to execute"},
    {"hint.set_mode_toggle",    "キー1回押しで開閉し、左クリックで実行", "Press once to toggle wheel open, left-click to execute"},
    {"hint.sl_drag",            "バーをクリックまたは左右にドラッグして調整", "Click or drag horizontally to adjust"},

    // Status messages
    {"ed.status.welcome",       "項目にカーソルを合わせると説明が表示されます  |  Ctrl+S 保存 / Ctrl+Z 戻す",
                                "Hover any control for help  |  Ctrl+S Save / Ctrl+Z Undo"},
    {"ed.status.saved",         "config.json に保存しました", "Saved to config.json"},
    {"ed.status.save_fail",     "config.json の保存に失敗しました", "Failed to save config.json"},
    {"ed.status.reloaded",      "config.json を再読み込みしました", "Reloaded config.json"},
    {"ed.status.undone",        "直前の変更を元に戻しました", "Undid last change"},
    {"ed.status.reset",         "初期設定に戻しました（保存で確定）", "Reset to defaults (click Save)"},
    {"ed.status.tested",        "選択スロットをテスト実行しました", "Executed slot test"},
};

} // namespace

std::string_view uiLanguageToString(UiLanguage lang) noexcept {
    switch (lang) {
    case UiLanguage::Auto: return "auto";
    case UiLanguage::Ja:   return "ja";
    case UiLanguage::En:   return "en";
    }
    return "auto";
}

std::optional<UiLanguage> parseUiLanguage(std::string_view sv) noexcept {
    std::string lo = toLowerTrim(sv);
    if (lo.empty() || lo == "auto" || lo == "system") return UiLanguage::Auto;
    if (lo == "ja" || lo == "jp" || lo == "ja_jp" || lo == "japanese") return UiLanguage::Ja;
    if (lo == "en" || lo == "en_us" || lo == "english") return UiLanguage::En;
    return std::nullopt;
}

ResolvedLang resolveUiLanguage(UiLanguage pref, bool systemIsJapanese) noexcept {
    if (pref == UiLanguage::Ja) return ResolvedLang::Ja;
    if (pref == UiLanguage::En) return ResolvedLang::En;
    return systemIsJapanese ? ResolvedLang::Ja : ResolvedLang::En;
}

char const* tr(ResolvedLang lang, std::string_view key) noexcept {
    for (auto const& p : kDict) {
        if (p.key == key) {
            return (lang == ResolvedLang::Ja) ? p.ja : p.en;
        }
    }
    return "";
}

std::string truncateUtf8Bytes(std::string_view sv, size_t maxBytes) {
    if (sv.size() <= maxBytes) return std::string(sv);
    size_t pos = 0;
    while (pos < sv.size()) {
        size_t len = utf8CharLen(static_cast<unsigned char>(sv[pos]));
        if (pos + len > sv.size() || pos + len > maxBytes) break;
        pos += len;
    }
    return std::string(sv.substr(0, pos));
}

std::string truncateUtf8Codepoints(std::string_view sv, size_t maxCodepoints) {
    std::string out;
    size_t      count = 0;
    size_t      i     = 0;
    while (i < sv.size()) {
        size_t len = utf8CharLen(static_cast<unsigned char>(sv[i]));
        if (i + len > sv.size()) break;
        if (count >= maxCodepoints) {
            out += "..";
            return out;
        }
        out.append(sv.data() + i, len);
        i += len;
        ++count;
    }
    return out;
}

float estimateDisplayWidth(std::string_view utf8, float fontScale) noexcept {
    if (utf8.empty() || !(fontScale > 0.0f)) return 0.0f;
    float  units = 0.0f;
    size_t i     = 0;
    while (i < utf8.size()) {
        // Skip Minecraft formatting code "§x" (0xC2 0xA7 + 1 ASCII byte).
        if (static_cast<unsigned char>(utf8[i]) == 0xC2u
            && i + 2 < utf8.size()
            && static_cast<unsigned char>(utf8[i + 1]) == 0xA7u) {
            i += 3;
            continue;
        }
        uint32_t cp = decodeUtf8(utf8, i);
        if (isWideCodepoint(cp)) {
            units += 1.85f;
        } else if (cp == ' ' || cp == 'i' || cp == 'l' || cp == '.' || cp == ':' || cp == '|' || cp == '!') {
            units += 0.62f;
        } else {
            units += 1.0f;
        }
    }
    return units * 5.6f * fontScale;
}

std::string fitTextToWidth(
    std::string_view          utf8,
    float                     maxWidthPx,
    float                     fontScale,
    TextWidthMeasureFn const& measure
) {
    if (utf8.empty() || !(maxWidthPx > 2.0f)) return "";
    auto widthOf = [&](std::string_view s) -> float {
        if (measure) {
            float w = measure(s);
            if (w > 0.0f) return w;
        }
        return estimateDisplayWidth(s, fontScale);
    };

    if (widthOf(utf8) <= maxWidthPx) {
        return std::string(utf8);
    }

    float const ellipsisW = widthOf("..");
    float const budget    = std::max(0.0f, maxWidthPx - ellipsisW);
    if (budget <= 1.0f) return "";

    // Collect valid UTF-8 prefix byte boundaries and binary-search the longest fitting prefix.
    std::vector<size_t> bounds;
    bounds.reserve(utf8.size() + 1);
    bounds.push_back(0);
    size_t i = 0;
    while (i < utf8.size()) {
        size_t len = utf8CharLen(static_cast<unsigned char>(utf8[i]));
        if (i + len > utf8.size()) break;
        i += len;
        bounds.push_back(i);
    }

    size_t lo = 0;
    size_t hi = bounds.size() - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (widthOf(utf8.substr(0, bounds[mid])) <= budget) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    std::string out(utf8.substr(0, bounds[lo]));
    out += "..";
    return out;
}

CaretTextSlice sliceTextForCaret(
    std::string_view          utf8,
    size_t                    cursorByte,
    float                     maxWidthPx,
    float                     fontScale,
    TextWidthMeasureFn const& measure
) {
    CaretTextSlice result;
    if (utf8.empty() || !(maxWidthPx > 2.0f)) return result;

    auto widthOf = [&](std::string_view s) -> float {
        if (s.empty()) return 0.0f;
        if (measure) {
            float w = measure(s);
            if (w > 0.0f) return w;
        }
        return estimateDisplayWidth(s, fontScale);
    };

    // Collect valid UTF-8 codepoint boundaries.
    std::vector<size_t> bounds;
    bounds.reserve(utf8.size() + 1);
    bounds.push_back(0);
    size_t i = 0;
    while (i < utf8.size()) {
        size_t len = utf8CharLen(static_cast<unsigned char>(utf8[i]));
        if (i + len > utf8.size()) break;
        i += len;
        bounds.push_back(i);
    }

    size_t clampedCursor = std::min(cursorByte, bounds.back());
    size_t cIdx          = 0;
    for (size_t k = 0; k < bounds.size(); ++k) {
        if (bounds[k] <= clampedCursor) cIdx = k;
        else break;
    }

    float const caretBudget = std::max(1.0f, maxWidthPx - 1.5f);

    // Find smallest startIdx in [0, cIdx] such that [startIdx .. cIdx] fits within caretBudget.
    size_t lo = 0;
    size_t hi = cIdx;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (widthOf(utf8.substr(bounds[mid], bounds[cIdx] - bounds[mid])) <= caretBudget) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    size_t const startIdx = lo;

    // Find largest endIdx in [cIdx, bounds.size() - 1] such that [startIdx .. endIdx] fits within maxWidthPx.
    lo = cIdx;
    hi = bounds.size() - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (widthOf(utf8.substr(bounds[startIdx], bounds[mid] - bounds[startIdx])) <= maxWidthPx) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    size_t const endIdx = lo;

    result.visibleText   = std::string(utf8.substr(bounds[startIdx], bounds[endIdx] - bounds[startIdx]));
    float const prefixPx = widthOf(utf8.substr(bounds[startIdx], bounds[cIdx] - bounds[startIdx]));
    result.caretOffsetPx = std::clamp(prefixPx, 0.0f, std::max(0.0f, maxWidthPx - 1.0f));
    return result;
}

size_t caretByteFromClickOffset(
    std::string_view          utf8,
    size_t                    prevCursorByte,
    float                     clickOffsetPx,
    float                     maxWidthPx,
    float                     fontScale,
    TextWidthMeasureFn const& measure
) {
    if (utf8.empty() || !(maxWidthPx > 2.0f)) return 0;

    auto widthOf = [&](std::string_view s) -> float {
        if (s.empty()) return 0.0f;
        if (measure) {
            float w = measure(s);
            if (w > 0.0f) return w;
        }
        return estimateDisplayWidth(s, fontScale);
    };

    std::vector<size_t> bounds;
    bounds.reserve(utf8.size() + 1);
    bounds.push_back(0);
    size_t i = 0;
    while (i < utf8.size()) {
        size_t len = utf8CharLen(static_cast<unsigned char>(utf8[i]));
        if (i + len > utf8.size()) break;
        i += len;
        bounds.push_back(i);
    }

    size_t clampedCursor = std::min(prevCursorByte, bounds.back());
    size_t cIdx          = 0;
    for (size_t k = 0; k < bounds.size(); ++k) {
        if (bounds[k] <= clampedCursor) cIdx = k;
        else break;
    }

    float const caretBudget = std::max(1.0f, maxWidthPx - 1.5f);
    size_t      lo          = 0;
    size_t      hi          = cIdx;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (widthOf(utf8.substr(bounds[mid], bounds[cIdx] - bounds[mid])) <= caretBudget) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    size_t const startIdx = lo;

    lo = cIdx;
    hi = bounds.size() - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (widthOf(utf8.substr(bounds[startIdx], bounds[mid] - bounds[startIdx])) <= maxWidthPx) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    size_t const endIdx = lo;

    if (clickOffsetPx <= 0.0f) return bounds[startIdx];

    size_t bestByte = bounds[startIdx];
    float  bestDist = clickOffsetPx;
    for (size_t k = startIdx + 1; k <= endIdx; ++k) {
        float px   = widthOf(utf8.substr(bounds[startIdx], bounds[k] - bounds[startIdx]));
        float dist = std::abs(px - clickOffsetPx);
        if (dist <= bestDist) {
            bestDist = dist;
            bestByte = bounds[k];
        } else if (px > clickOffsetPx) {
            break;
        }
    }
    return bestByte;
}

} // namespace radial
