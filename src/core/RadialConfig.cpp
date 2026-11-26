#include "core/RadialConfig.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <variant>

namespace radial {
namespace {

std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string trimAscii(std::string_view sv) {
    size_t b = 0;
    size_t e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

int safeDoubleToInt(double v, int fallback = 0) noexcept {
    if (!std::isfinite(v)) return fallback;
    double lo = static_cast<double>(std::numeric_limits<int>::min());
    double hi = static_cast<double>(std::numeric_limits<int>::max());
    double cl = std::clamp(std::round(v), lo, hi);
    return static_cast<int>(cl);
}

// ============================================================================
// Lightweight locale-independent JSONC (comments + trailing commas) parser
// ============================================================================

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;
using JsonArray  = std::vector<JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data{nullptr};

    bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool() const noexcept { return std::holds_alternative<bool>(data); }
    bool isNumber() const noexcept { return std::holds_alternative<double>(data); }
    bool isString() const noexcept { return std::holds_alternative<std::string>(data); }
    bool isArray() const noexcept { return std::holds_alternative<JsonArray>(data); }
    bool isObject() const noexcept { return std::holds_alternative<JsonObject>(data); }

    bool               asBool() const noexcept { return std::get<bool>(data); }
    double             asNumber() const noexcept { return std::get<double>(data); }
    std::string const& asString() const noexcept { return std::get<std::string>(data); }
    JsonArray const&   asArray() const noexcept { return std::get<JsonArray>(data); }
    JsonObject const&  asObject() const noexcept { return std::get<JsonObject>(data); }

    // Last-wins lookup (matches standard JSON / JavaScript semantics).
    JsonValue const* find(std::string_view key) const noexcept {
        if (!isObject()) return nullptr;
        auto const& obj = asObject();
        for (size_t i = obj.size(); i > 0; --i) {
            if (obj[i - 1].first == key) return &obj[i - 1].second;
        }
        return nullptr;
    }
};

class JsonParser {
    std::string_view mSrc;
    size_t           mPos   = 0;
    std::string      mError;
    int              mDepth = 0;
    size_t           mNodes = 0;

    static constexpr int    kMaxDepth = 64;
    static constexpr size_t kMaxNodes = 25000;

    bool skipWsAndComments() noexcept {
        while (mPos < mSrc.size()) {
            char c = mSrc[mPos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++mPos;
                continue;
            }
            if (c == '/' && mPos + 1 < mSrc.size()) {
                if (mSrc[mPos + 1] == '/') {
                    mPos += 2;
                    while (mPos < mSrc.size() && mSrc[mPos] != '\n') ++mPos;
                    continue;
                }
                if (mSrc[mPos + 1] == '*') {
                    mPos += 2;
                    bool closed = false;
                    while (mPos + 1 < mSrc.size()) {
                        if (mSrc[mPos] == '*' && mSrc[mPos + 1] == '/') {
                            mPos += 2;
                            closed = true;
                            break;
                        }
                        ++mPos;
                    }
                    if (!closed) {
                        mError = "unterminated block comment";
                        return false;
                    }
                    continue;
                }
            }
            break;
        }
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool parseHex4(uint32_t& out) noexcept {
        if (mPos + 4 > mSrc.size()) return false;
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            char c = mSrc[mPos + i];
            val <<= 4;
            if (c >= '0' && c <= '9') val |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') val |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        mPos += 4;
        out = val;
        return true;
    }

    bool parseString(std::string& out) {
        if (mPos >= mSrc.size() || mSrc[mPos] != '"') {
            mError = "expected '\"'";
            return false;
        }
        ++mPos;
        out.clear();
        while (mPos < mSrc.size()) {
            char c = mSrc[mPos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (mPos >= mSrc.size()) {
                    mError = "unterminated escape";
                    return false;
                }
                char esc = mSrc[mPos++];
                switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parseHex4(cp)) {
                        mError = "invalid \\u escape";
                        return false;
                    }
                    if (cp == 0u) {
                        mError = "NUL character not allowed in string";
                        return false;
                    }
                    if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        mError = "lone low UTF-16 surrogate";
                        return false;
                    }
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        if (!(mPos + 2 <= mSrc.size() && mSrc[mPos] == '\\' && mSrc[mPos + 1] == 'u')) {
                            mError = "lone high UTF-16 surrogate";
                            return false;
                        }
                        mPos += 2;
                        uint32_t low = 0;
                        if (!parseHex4(low) || low < 0xDC00u || low > 0xDFFFu) {
                            mError = "invalid UTF-16 surrogate pair";
                            return false;
                        }
                        cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    mError = "unknown escape sequence";
                    return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20u || static_cast<unsigned char>(c) == 0x7Fu) {
                mError = "unescaped control character in string";
                return false;
            } else {
                out.push_back(c);
            }
        }
        mError = "unterminated string";
        return false;
    }

    bool parseNumber(double& out) {
        size_t start = mPos;
        if (mPos < mSrc.size() && mSrc[mPos] == '-') ++mPos;
        if (mPos >= mSrc.size() || !std::isdigit(static_cast<unsigned char>(mSrc[mPos]))) {
            mError = "invalid number";
            return false;
        }
        if (mSrc[mPos] == '0') {
            ++mPos;
            if (mPos < mSrc.size() && std::isdigit(static_cast<unsigned char>(mSrc[mPos]))) {
                mError = "leading zero in number";
                return false;
            }
        } else {
            while (mPos < mSrc.size() && std::isdigit(static_cast<unsigned char>(mSrc[mPos]))) ++mPos;
        }
        if (mPos < mSrc.size() && mSrc[mPos] == '.') {
            ++mPos;
            if (mPos >= mSrc.size() || !std::isdigit(static_cast<unsigned char>(mSrc[mPos]))) {
                mError = "invalid decimal number";
                return false;
            }
            while (mPos < mSrc.size() && std::isdigit(static_cast<unsigned char>(mSrc[mPos]))) ++mPos;
        }
        if (mPos < mSrc.size() && (mSrc[mPos] == 'e' || mSrc[mPos] == 'E')) {
            ++mPos;
            if (mPos < mSrc.size() && (mSrc[mPos] == '+' || mSrc[mPos] == '-')) ++mPos;
            if (mPos >= mSrc.size() || !std::isdigit(static_cast<unsigned char>(mSrc[mPos]))) {
                mError = "invalid exponent";
                return false;
            }
            while (mPos < mSrc.size() && std::isdigit(static_cast<unsigned char>(mSrc[mPos]))) ++mPos;
        }

        // Locale-independent parsing via std::from_chars.
        char const* pBegin = mSrc.data() + start;
        char const* pEnd   = mSrc.data() + mPos;
        double      v      = 0.0;
        auto        res    = std::from_chars(pBegin, pEnd, v);
        if (res.ec != std::errc{} || res.ptr != pEnd || !std::isfinite(v)) {
            mError = "number out of range or non-finite";
            return false;
        }
        out = v;
        return true;
    }

    bool parseValue(JsonValue& out) {
        if (++mNodes > kMaxNodes) {
            mError = "JSON exceeds maximum node limit";
            return false;
        }
        if (++mDepth > kMaxDepth) {
            mError = "JSON nesting too deep";
            return false;
        }
        struct DepthGuard {
            int& d;
            ~DepthGuard() { --d; }
        } guard{mDepth};

        if (!skipWsAndComments()) return false;
        if (mPos >= mSrc.size()) {
            mError = "unexpected end of JSON";
            return false;
        }
        char c = mSrc[mPos];
        if (c == '{') {
            ++mPos;
            JsonObject obj;
            if (!skipWsAndComments()) return false;
            if (mPos < mSrc.size() && mSrc[mPos] == '}') {
                ++mPos;
                out.data = std::move(obj);
                return true;
            }
            while (mPos < mSrc.size()) {
                if (!skipWsAndComments()) return false;
                if (mPos < mSrc.size() && mSrc[mPos] == '}') {
                    ++mPos; // trailing comma allowed
                    out.data = std::move(obj);
                    return true;
                }
                std::string key;
                if (!parseString(key)) return false;
                if (!skipWsAndComments()) return false;
                if (mPos >= mSrc.size() || mSrc[mPos] != ':') {
                    mError = "expected ':' after object key";
                    return false;
                }
                ++mPos;
                JsonValue val;
                if (!parseValue(val)) return false;
                obj.emplace_back(std::move(key), std::move(val));
                if (!skipWsAndComments()) return false;
                if (mPos < mSrc.size() && mSrc[mPos] == ',') {
                    ++mPos;
                    continue;
                }
                if (mPos < mSrc.size() && mSrc[mPos] == '}') {
                    ++mPos;
                    out.data = std::move(obj);
                    return true;
                }
                mError = "expected ',' or '}' in object";
                return false;
            }
            mError = "unterminated object";
            return false;
        }
        if (c == '[') {
            ++mPos;
            JsonArray arr;
            if (!skipWsAndComments()) return false;
            if (mPos < mSrc.size() && mSrc[mPos] == ']') {
                ++mPos;
                out.data = std::move(arr);
                return true;
            }
            while (mPos < mSrc.size()) {
                if (!skipWsAndComments()) return false;
                if (mPos < mSrc.size() && mSrc[mPos] == ']') {
                    ++mPos; // trailing comma allowed
                    out.data = std::move(arr);
                    return true;
                }
                JsonValue val;
                if (!parseValue(val)) return false;
                arr.push_back(std::move(val));
                if (!skipWsAndComments()) return false;
                if (mPos < mSrc.size() && mSrc[mPos] == ',') {
                    ++mPos;
                    continue;
                }
                if (mPos < mSrc.size() && mSrc[mPos] == ']') {
                    ++mPos;
                    out.data = std::move(arr);
                    return true;
                }
                mError = "expected ',' or ']' in array";
                return false;
            }
            mError = "unterminated array";
            return false;
        }
        if (c == '"') {
            std::string s;
            if (!parseString(s)) return false;
            out.data = std::move(s);
            return true;
        }
        if (c == 't' && mSrc.substr(mPos, 4) == "true") {
            mPos += 4;
            out.data = true;
            return true;
        }
        if (c == 'f' && mSrc.substr(mPos, 5) == "false") {
            mPos += 5;
            out.data = false;
            return true;
        }
        if (c == 'n' && mSrc.substr(mPos, 4) == "null") {
            mPos += 4;
            out.data = nullptr;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            double v = 0.0;
            if (!parseNumber(v)) return false;
            out.data = v;
            return true;
        }
        mError = "unexpected character in JSON";
        return false;
    }

public:
    explicit JsonParser(std::string_view src) : mSrc(src) {
        if (mSrc.size() >= 3 && static_cast<unsigned char>(mSrc[0]) == 0xEFu
            && static_cast<unsigned char>(mSrc[1]) == 0xBBu
            && static_cast<unsigned char>(mSrc[2]) == 0xBFu) {
            mPos = 3;
        }
    }

    bool parse(JsonValue& out) {
        if (!parseValue(out)) return false;
        if (!skipWsAndComments()) return false;
        if (mPos != mSrc.size()) {
            mError = "trailing characters after JSON root";
            return false;
        }
        return true;
    }

    std::string const& error() const noexcept { return mError; }
};

std::string jsonEscape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 2);
    out.push_back('"');
    for (unsigned char c : sv) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20u) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string indentStr(int level) {
    return std::string(static_cast<size_t>(level) * 2, ' ');
}

void appendWarning(std::string* warn, std::string_view msg) {
    if (!warn || msg.empty()) return;
    if (!warn->empty()) *warn += "; ";
    *warn += msg;
}

float safeFiniteFloat(float v, float fallback) noexcept {
    return std::isfinite(v) ? v : fallback;
}

void sanitizeSlot(RadialSlot& slot, int depth) noexcept {
    slot.label    = truncateUtf8Bytes(slot.label, 48);
    slot.subtitle = truncateUtf8Bytes(slot.subtitle, 64);
    slot.value    = truncateUtf8Bytes(slot.value, 512);
    if (!slot.color.empty() && !parseHexColor(slot.color).has_value()) {
        slot.color.clear();
    }
    if (slot.cooldownMs < -1) slot.cooldownMs = -1;
    if (slot.cooldownMs > 60000) slot.cooldownMs = 60000;

    if (slot.type == ActionType::Submenu && depth >= kMaxSubmenuDepth) {
        slot.type = ActionType::Command;
        slot.children.clear();
    }

    if (slot.type == ActionType::Macro) {
        if (slot.steps.size() > static_cast<size_t>(kMaxMacroSteps)) {
            slot.steps.resize(static_cast<size_t>(kMaxMacroSteps));
        }
        for (auto& st : slot.steps) {
            if (st.type == ActionType::Macro || st.type == ActionType::Submenu) {
                st.type = ActionType::Command;
            }
            st.value   = truncateUtf8Bytes(st.value, 512);
            st.delayMs = std::clamp(st.delayMs, 0, 10000);
        }
    }

    if (slot.type == ActionType::Submenu) {
        if (slot.children.size() > static_cast<size_t>(kMaxSlotsPerMenu)) {
            slot.children.resize(static_cast<size_t>(kMaxSlotsPerMenu));
        }
        for (auto& child : slot.children) {
            sanitizeSlot(child, depth + 1);
        }
    }

    if (slot.label.empty()) {
        if (!slot.value.empty()) {
            slot.label = truncateUtf8Bytes(slot.value, 18);
        } else if (slot.type == ActionType::Submenu) {
            slot.label = "Submenu";
        } else if (slot.type == ActionType::Macro) {
            slot.label = "Macro";
        } else {
            slot.label = "Slot";
        }
    }
}

void parseSlotObject(JsonValue const& v, RadialSlot& out, int depth, std::string* warn) {
    if (!v.isObject()) return;
    if (auto* f = v.find("label"); f && f->isString()) out.label = f->asString();
    if (auto* f = v.find("subtitle"); f && f->isString()) out.subtitle = f->asString();
    if (auto* f = v.find("type"); f && f->isString()) {
        if (auto parsed = parseActionType(f->asString())) {
            out.type = *parsed;
        } else {
            appendWarning(warn, "unknown slot type '" + f->asString() + "'");
        }
    }
    if (auto* f = v.find("value"); f && f->isString()) out.value = f->asString();
    if (out.value.empty()) {
        if (auto* f = v.find("command"); f && f->isString()) {
            out.value = f->asString();
            if (!v.find("type")) out.type = ActionType::Command;
        } else if (auto* f2 = v.find("chat"); f2 && f2->isString()) {
            out.value = f2->asString();
            if (!v.find("type")) out.type = ActionType::Chat;
        }
    }
    if (auto* f = v.find("color"); f && f->isString()) out.color = f->asString();
    if (auto* f = v.find("cooldownMs"); f && f->isNumber()) {
        out.cooldownMs = safeDoubleToInt(f->asNumber(), -1);
    }
    if (auto* f = v.find("steps"); f && f->isArray()) {
        out.steps.clear();
        for (auto const& sv : f->asArray()) {
            if (!sv.isObject()) continue;
            MacroStep st;
            if (auto* sf = sv.find("type"); sf && sf->isString()) {
                if (auto parsed = parseActionType(sf->asString())) st.type = *parsed;
            }
            if (auto* sf = sv.find("value"); sf && sf->isString()) st.value = sf->asString();
            if (auto* sf = sv.find("delayMs"); sf && sf->isNumber()) {
                st.delayMs = safeDoubleToInt(sf->asNumber(), 0);
            }
            if (!st.value.empty()) out.steps.push_back(std::move(st));
        }
        if (!v.find("type") && !out.steps.empty()) out.type = ActionType::Macro;
    }
    if (auto* f = v.find("children"); f && f->isArray() && depth < kMaxSubmenuDepth) {
        out.children.clear();
        for (auto const& cv : f->asArray()) {
            if (!cv.isObject()) continue;
            RadialSlot child;
            parseSlotObject(cv, child, depth + 1, warn);
            out.children.push_back(std::move(child));
        }
        if (!v.find("type") && !out.children.empty()) out.type = ActionType::Submenu;
    }
}

void serializeSlot(std::ostringstream& os, RadialSlot const& s, int indent) {
    std::string ind  = indentStr(indent);
    std::string ind2 = indentStr(indent + 1);
    os << ind << "{\n";
    os << ind2 << "\"label\": " << jsonEscape(s.label) << ",\n";
    if (!s.subtitle.empty()) {
        os << ind2 << "\"subtitle\": " << jsonEscape(s.subtitle) << ",\n";
    }
    os << ind2 << "\"type\": " << jsonEscape(actionTypeToString(s.type));
    if (s.type == ActionType::Command || s.type == ActionType::Chat || s.type == ActionType::ClientCommand) {
        os << ",\n" << ind2 << "\"value\": " << jsonEscape(s.value);
    }
    if (!s.color.empty()) {
        os << ",\n" << ind2 << "\"color\": " << jsonEscape(s.color);
    }
    if (s.cooldownMs >= 0) {
        os << ",\n" << ind2 << "\"cooldownMs\": " << s.cooldownMs;
    }
    if (s.type == ActionType::Macro) {
        os << ",\n" << ind2 << "\"steps\": [";
        if (!s.steps.empty()) os << "\n";
        for (size_t i = 0; i < s.steps.size(); ++i) {
            auto const& st = s.steps[i];
            os << indentStr(indent + 2) << "{ \"type\": " << jsonEscape(actionTypeToString(st.type))
               << ", \"value\": " << jsonEscape(st.value)
               << ", \"delayMs\": " << st.delayMs << " }";
            if (i + 1 < s.steps.size()) os << ",";
            os << "\n";
        }
        os << (s.steps.empty() ? "" : ind2) << "]";
    }
    if (s.type == ActionType::Submenu) {
        os << ",\n" << ind2 << "\"children\": [";
        if (!s.children.empty()) os << "\n";
        for (size_t i = 0; i < s.children.size(); ++i) {
            serializeSlot(os, s.children[i], indent + 2);
            if (i + 1 < s.children.size()) os << ",";
            os << "\n";
        }
        os << (s.children.empty() ? "" : ind2) << "]";
    }
    os << "\n" << ind << "}";
}

} // namespace

std::string_view activationModeToString(ActivationMode m) noexcept {
    switch (m) {
    case ActivationMode::Hold:   return "hold";
    case ActivationMode::Toggle: return "toggle";
    }
    return "hold";
}

std::optional<ActivationMode> parseActivationMode(std::string_view sv) noexcept {
    std::string lo = toLowerAscii(trimAscii(sv));
    if (lo == "hold") return ActivationMode::Hold;
    if (lo == "toggle" || lo == "tap") return ActivationMode::Toggle;
    return std::nullopt;
}

std::string_view actionTypeToString(ActionType t) noexcept {
    switch (t) {
    case ActionType::Command:       return "command";
    case ActionType::Chat:          return "chat";
    case ActionType::ClientCommand: return "client_command";
    case ActionType::Macro:         return "macro";
    case ActionType::Submenu:       return "submenu";
    }
    return "command";
}

std::optional<ActionType> parseActionType(std::string_view sv) noexcept {
    std::string lo = toLowerAscii(trimAscii(sv));
    if (lo == "command" || lo == "cmd") return ActionType::Command;
    if (lo == "chat" || lo == "say" || lo == "message") return ActionType::Chat;
    if (lo == "client_command" || lo == "clientcommand" || lo == "client") return ActionType::ClientCommand;
    if (lo == "macro" || lo == "sequence") return ActionType::Macro;
    if (lo == "submenu" || lo == "menu" || lo == "folder") return ActionType::Submenu;
    return std::nullopt;
}

std::string_view connectionScopeToString(ConnectionScope s) noexcept {
    switch (s) {
    case ConnectionScope::Any:      return "any";
    case ConnectionScope::External: return "external";
    case ConnectionScope::Local:    return "local";
    }
    return "any";
}

std::optional<ConnectionScope> parseConnectionScope(std::string_view sv) noexcept {
    std::string lo = toLowerAscii(trimAscii(sv));
    if (lo == "any" || lo == "all" || lo.empty()) return ConnectionScope::Any;
    if (lo == "external" || lo == "server" || lo == "multiplayer") return ConnectionScope::External;
    if (lo == "local" || lo == "singleplayer" || lo == "world") return ConnectionScope::Local;
    return std::nullopt;
}

bool keyMatchesBinding(int incomingVk, int bindingVk) noexcept {
    if (bindingVk <= 0 || incomingVk <= 0) return false;
    if (incomingVk == bindingVk) return true;
    auto isAlt   = [](int k) { return k == kVkMenu || k == kVkLMenu || k == kVkRMenu; };
    auto isShift = [](int k) { return k == kVkShift || k == kVkLShift || k == kVkRShift; };
    auto isCtrl  = [](int k) { return k == kVkControl || k == kVkLControl || k == kVkRControl; };
    if (bindingVk == kVkMenu && isAlt(incomingVk)) return true;
    if (incomingVk == kVkMenu && isAlt(bindingVk)) return true;
    if (bindingVk == kVkShift && isShift(incomingVk)) return true;
    if (incomingVk == kVkShift && isShift(bindingVk)) return true;
    if (bindingVk == kVkControl && isCtrl(incomingVk)) return true;
    if (incomingVk == kVkControl && isCtrl(bindingVk)) return true;
    return false;
}

bool isMouseButtonVk(int vk) noexcept {
    return vk == kVkMButton || vk == kVkXButton1 || vk == kVkXButton2;
}

std::string keyCodeToName(int vk) {
    if (vk <= 0) return "None";
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    if (vk >= kVkF1 && vk <= kVkF12) return "F" + std::to_string(vk - kVkF1 + 1);
    switch (vk) {
    case kVkMenu:     return "Alt";
    case kVkLMenu:    return "LAlt";
    case kVkRMenu:    return "RAlt";
    case kVkTab:      return "Tab";
    case kVkShift:    return "Shift";
    case kVkLShift:   return "LShift";
    case kVkRShift:   return "RShift";
    case kVkControl:  return "Ctrl";
    case kVkLControl: return "LCtrl";
    case kVkRControl: return "RCtrl";
    case kVkCapital:  return "CapsLock";
    case kVkSpace:    return "Space";
    case kVkInsert:   return "Insert";
    case kVkHome:     return "Home";
    case kVkEnd:      return "End";
    case kVkOem3:     return "Grave";
    case kVkMButton:  return "MouseMiddle";
    case kVkXButton1: return "Mouse4";
    case kVkXButton2: return "Mouse5";
    default: {
        char buf[16]{};
        std::snprintf(buf, sizeof(buf), "VK_%d", vk);
        return buf;
    }
    }
}

std::optional<int> parseKeyName(std::string_view name) noexcept {
    std::string lo = toLowerAscii(trimAscii(name));
    if (lo.empty()) return std::nullopt;
    if (lo == "none" || lo == "disabled" || lo == "off") return 0;
    if (lo.size() == 1) {
        char c = lo[0];
        if (c >= 'a' && c <= 'z') return static_cast<int>('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<int>(c);
        if (c == '`' || c == '~') return kVkOem3;
    }
    auto parseStrictInt = [](std::string_view sv) noexcept -> std::optional<int> {
        if (sv.empty()) return std::nullopt;
        for (char c : sv) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        }
        int  val = 0;
        auto res = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size()) return std::nullopt;
        return val;
    };
    if (lo.size() >= 2 && lo[0] == 'f') {
        if (auto n = parseStrictInt(std::string_view(lo).substr(1)); n && *n >= 1 && *n <= 12) {
            return kVkF1 + (*n - 1);
        }
    }
    if (lo == "alt" || lo == "menu") return kVkMenu;
    if (lo == "lalt" || lo == "leftalt" || lo == "lmenu") return kVkLMenu;
    if (lo == "ralt" || lo == "rightalt" || lo == "rmenu") return kVkRMenu;
    if (lo == "tab") return kVkTab;
    if (lo == "shift") return kVkShift;
    if (lo == "lshift" || lo == "leftshift") return kVkLShift;
    if (lo == "rshift" || lo == "rightshift") return kVkRShift;
    if (lo == "ctrl" || lo == "control") return kVkControl;
    if (lo == "lctrl" || lo == "leftctrl") return kVkLControl;
    if (lo == "rctrl" || lo == "rightctrl") return kVkRControl;
    if (lo == "capslock" || lo == "caps") return kVkCapital;
    if (lo == "space") return kVkSpace;
    if (lo == "insert" || lo == "ins") return kVkInsert;
    if (lo == "home") return kVkHome;
    if (lo == "end") return kVkEnd;
    if (lo == "grave" || lo == "backquote" || lo == "tilde") return kVkOem3;
    if (lo == "mousemiddle" || lo == "mmb" || lo == "middlemouse" || lo == "mouse3") return kVkMButton;
    if (lo == "mouse4" || lo == "xbutton1" || lo == "mb4" || lo == "side1") return kVkXButton1;
    if (lo == "mouse5" || lo == "xbutton2" || lo == "mb5" || lo == "side2") return kVkXButton2;
    if (lo.starts_with("vk_")) {
        if (auto code = parseStrictInt(std::string_view(lo).substr(3)); code && *code > 0 && *code < 255) {
            return *code;
        }
        return std::nullopt;
    }
    if (auto code = parseStrictInt(lo); code && *code >= 0 && *code < 255) {
        return *code;
    }
    return std::nullopt;
}

std::optional<uint32_t> parseHexColor(std::string_view hex) noexcept {
    std::string s = trimAscii(hex);
    if (s.size() == 7 && s[0] == '#') s = s.substr(1);
    if (s.size() != 6) return std::nullopt;
    uint32_t val = 0;
    for (char c : s) {
        val <<= 4;
        if (c >= '0' && c <= '9') val |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') val |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= static_cast<uint32_t>(c - 'A' + 10);
        else return std::nullopt;
    }
    return val;
}

RadialConfig makeDefaultConfig() {
    RadialConfig cfg;
    cfg.version         = 1;
    cfg.enabled         = true;
    cfg.language        = UiLanguage::Auto;
    cfg.activationMode  = ActivationMode::Hold;
    cfg.activationKey   = kVkMenu; // Alt
    cfg.editorKey       = kVkF12;  // F12
    cfg.defaultPresetId = "default";

    RadialSlot quickChatSub;
    quickChatSub.label    = "Quick Chat";
    quickChatSub.subtitle = "Submenu";
    quickChatSub.type     = ActionType::Submenu;
    quickChatSub.color    = "#58A6FF";
    quickChatSub.children = {
        RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#3FB950"},
        RadialSlot{"GLHF", "Chat", ActionType::Chat, "glhf!", "#3FB950"},
        RadialSlot{"Yoroshiku", "Chat", ActionType::Chat, "よろしく！", "#58A6FF"},
        RadialSlot{"Thanks", "Chat", ActionType::Chat, "ありがとう！", "#58A6FF"},
        RadialSlot{"Nice!", "Chat", ActionType::Chat, "ナイス！", "#F0B429"},
        RadialSlot{"Coords", "Share XYZ", ActionType::Chat, "XYZ: {x} {y} {z}", "#A78BFA"},
    };

    RadialSlot ggHubMacro;
    ggHubMacro.label      = "GG + Hub";
    ggHubMacro.subtitle   = "Macro";
    ggHubMacro.type       = ActionType::Macro;
    ggHubMacro.color      = "#D2A8FF";
    ggHubMacro.cooldownMs = 1500;
    ggHubMacro.steps      = {
        MacroStep{ActionType::Chat, "gg", 0},
        MacroStep{ActionType::Command, "/hub", 250},
    };

    // 1) Default fallback preset
    RadialPreset defPreset;
    defPreset.id   = "default";
    defPreset.name = "Default";
    defPreset.slots = {
        RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"},
        quickChatSub,
        RadialSlot{"Spawn", "/spawn", ActionType::Command, "/spawn", "#34D399"},
        RadialSlot{"Home", "/home", ActionType::Command, "/home", "#34D399"},
        ggHubMacro,
        RadialSlot{"Lobby", "/lobby", ActionType::Command, "/lobby", "#38BDF8"},
        RadialSlot{"Status", "/rc status", ActionType::ClientCommand, "/rc status", "#94A3B8"},
        RadialSlot{"Editor", "/rc editor", ActionType::ClientCommand, "/rc editor", "#F59E0B"},
    };

    // 2) The Hive preset
    RadialPreset hivePreset;
    hivePreset.id   = "hive";
    hivePreset.name = "The Hive";
    hivePreset.serverRules = {
        ServerRule{"*.hivebedrock.network", 0, "", "", ConnectionScope::External, 100},
        ServerRule{"", 0, "*Hive*", "", ConnectionScope::External, 90},
    };
    RadialSlot hiveParty;
    hiveParty.label    = "Party";
    hiveParty.subtitle = "Submenu";
    hiveParty.type     = ActionType::Submenu;
    hiveParty.color    = "#F59E0B";
    hiveParty.children = {
        RadialSlot{"List", "/p list", ActionType::Command, "/p list", "#F59E0B"},
        RadialSlot{"Warp", "/p warp", ActionType::Command, "/p warp", "#38BDF8"},
        RadialSlot{"Leave", "/p leave", ActionType::Command, "/p leave", "#F87171"},
        RadialSlot{"Friends", "/f list", ActionType::Command, "/f list", "#34D399"},
    };
    hivePreset.slots = {
        RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"},
        RadialSlot{"Requeue", "/q", ActionType::Command, "/q", "#34D399"},
        quickChatSub,
        ggHubMacro,
        hiveParty,
        RadialSlot{"Locker", "/locker", ActionType::Command, "/locker", "#A78BFA"},
        RadialSlot{"Replay", "/replay", ActionType::Command, "/replay", "#60A5FA"},
        RadialSlot{"Editor", "/rc editor", ActionType::ClientCommand, "/rc editor", "#F59E0B"},
    };

    // 3) CubeCraft preset
    RadialPreset cubePreset;
    cubePreset.id   = "cubecraft";
    cubePreset.name = "CubeCraft";
    cubePreset.serverRules = {
        ServerRule{"*.cubecraft.net", 0, "", "", ConnectionScope::External, 100},
        ServerRule{"", 0, "*CubeCraft*", "", ConnectionScope::External, 90},
    };
    cubePreset.slots = {
        RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"},
        RadialSlot{"Leave", "/leave", ActionType::Command, "/leave", "#F87171"},
        quickChatSub,
        ggHubMacro,
        RadialSlot{"Party", "/party", ActionType::Command, "/party", "#F59E0B"},
        RadialSlot{"Editor", "/rc editor", ActionType::ClientCommand, "/rc editor", "#94A3B8"},
    };

    // 4) Local World preset (safe, practical creative/survival helpers)
    RadialPreset localPreset;
    localPreset.id   = "local";
    localPreset.name = "Local World";
    localPreset.serverRules = {
        ServerRule{"", 0, "", "", ConnectionScope::Local, 10},
    };
    RadialSlot timeWeatherSub;
    timeWeatherSub.label    = "Time / Weather";
    timeWeatherSub.subtitle = "Submenu";
    timeWeatherSub.type     = ActionType::Submenu;
    timeWeatherSub.color    = "#F59E0B";
    timeWeatherSub.children = {
        RadialSlot{"Day", "/time set day", ActionType::Command, "/time set day", "#FBBF24"},
        RadialSlot{"Noon", "/time set noon", ActionType::Command, "/time set noon", "#F59E0B"},
        RadialSlot{"Night", "/time set night", ActionType::Command, "/time set night", "#6366F1"},
        RadialSlot{"Clear", "/weather clear", ActionType::Command, "/weather clear", "#38BDF8"},
    };
    localPreset.slots = {
        RadialSlot{"Creative", "/gamemode c", ActionType::Command, "/gamemode c", "#38BDF8"},
        RadialSlot{"Survival", "/gamemode s", ActionType::Command, "/gamemode s", "#34D399"},
        RadialSlot{"Spectator", "/gamemode spectator", ActionType::Command, "/gamemode spectator", "#A78BFA"},
        timeWeatherSub,
        RadialSlot{"Night Vision", "Effect", ActionType::Command, "/effect @s night_vision 600 0 true", "#60A5FA"},
        RadialSlot{"KeepInv On", "gamerule", ActionType::Command, "/gamerule keepinventory true", "#34D399"},
        RadialSlot{"Share XYZ", "Chat", ActionType::Chat, "XYZ: {x} {y} {z}", "#58A6FF"},
        RadialSlot{"Editor", "/rc editor", ActionType::ClientCommand, "/rc editor", "#F59E0B"},
    };

    cfg.presets = {
        std::move(defPreset),
        std::move(hivePreset),
        std::move(cubePreset),
        std::move(localPreset),
    };
    sanitizeConfig(cfg);
    return cfg;
}

void sanitizeConfig(RadialConfig& cfg) noexcept {
    cfg.version           = std::max(1, cfg.version);
    if (cfg.activationKey <= 0 || cfg.activationKey >= 255) cfg.activationKey = kVkMenu;
    if (cfg.editorKey < 0 || cfg.editorKey >= 255) cfg.editorKey = kVkF12;
    cfg.flickSensitivity  = std::clamp(safeFiniteFloat(cfg.flickSensitivity, 1.0f), 0.2f, 4.0f);
    cfg.outerRadius       = std::clamp(safeFiniteFloat(cfg.outerRadius, 92.0f), 50.0f, 180.0f);
    cfg.innerRadius       = std::clamp(safeFiniteFloat(cfg.innerRadius, 32.0f), 14.0f, std::min(85.0f, cfg.outerRadius - 20.0f));
    cfg.deadzoneRadius    = std::clamp(safeFiniteFloat(cfg.deadzoneRadius, 22.0f), 8.0f, std::min(80.0f, cfg.outerRadius - 12.0f));
    cfg.backgroundOpacity = std::clamp(safeFiniteFloat(cfg.backgroundOpacity, 0.78f), 0.15f, 1.0f);
    cfg.fontScale         = std::clamp(safeFiniteFloat(cfg.fontScale, 0.95f), 0.5f, 1.8f);
    cfg.holdDelayMs       = std::clamp(cfg.holdDelayMs, 0, 500);
    cfg.submenuHoverMs    = std::clamp(cfg.submenuHoverMs, 0, 1500);
    cfg.globalCooldownMs  = std::clamp(cfg.globalCooldownMs, 0, 10000);

    // Guarantee at least one execution path in Hold mode.
    if (cfg.activationMode == ActivationMode::Hold && !cfg.executeOnRelease && !cfg.executeOnLeftClick) {
        cfg.executeOnRelease = true;
    }

    if (cfg.presets.empty()) {
        cfg.presets = makeDefaultConfig().presets;
    }
    if (cfg.presets.size() > static_cast<size_t>(kMaxPresets)) {
        cfg.presets.resize(static_cast<size_t>(kMaxPresets));
    }

    std::set<std::string> seenIds;
    for (size_t i = 0; i < cfg.presets.size(); ++i) {
        auto& p = cfg.presets[i];
        p.id   = truncateUtf8Bytes(trimAscii(p.id), 48);
        p.name = truncateUtf8Bytes(p.name, 48);
        if (p.id.empty()) p.id = "preset_" + std::to_string(i + 1);
        std::string loId = toLowerAscii(p.id);
        if (seenIds.contains(loId)) {
            int suffix = 2;
            while (seenIds.contains(toLowerAscii(p.id + "_" + std::to_string(suffix)))) ++suffix;
            p.id += "_" + std::to_string(suffix);
            loId = toLowerAscii(p.id);
        }
        seenIds.insert(loId);

        if (p.name.empty()) p.name = p.id;
        if (p.serverRules.size() > static_cast<size_t>(kMaxRulesPerPreset)) {
            p.serverRules.resize(static_cast<size_t>(kMaxRulesPerPreset));
        }
        for (auto& r : p.serverRules) {
            r.hostPattern       = truncateUtf8Bytes(r.hostPattern, 128);
            r.serverNamePattern = truncateUtf8Bytes(r.serverNamePattern, 128);
            r.worldNamePattern  = truncateUtf8Bytes(r.worldNamePattern, 128);
            if (r.port < 0 || r.port > 65535) r.port = 0;
            r.priority = std::clamp(r.priority, -1000, 1000);
        }
        if (p.slots.size() > static_cast<size_t>(kMaxSlotsPerMenu)) {
            p.slots.resize(static_cast<size_t>(kMaxSlotsPerMenu));
        }
        for (auto& s : p.slots) {
            sanitizeSlot(s, 0);
        }
        if (p.slots.empty()) {
            p.slots.push_back(RadialSlot{"Hub", "/hub", ActionType::Command, "/hub", "#38BDF8"});
            p.slots.push_back(RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#3FB950"});
        } else if (p.slots.size() == 1) {
            p.slots.push_back(RadialSlot{"GG", "Chat", ActionType::Chat, "gg", "#3FB950"});
        }
    }

    if (findPresetIndex(cfg, cfg.defaultPresetId) < 0) {
        cfg.defaultPresetId = cfg.presets.front().id;
    }
}

bool parseConfigJson(std::string_view jsonText, RadialConfig& outConfig, std::string* outWarning) {
    if (jsonText.size() > kMaxConfigFileSize) {
        appendWarning(outWarning, "config file exceeds 2 MiB limit");
        return false;
    }
    JsonParser parser(jsonText);
    JsonValue  root;
    if (!parser.parse(root) || !root.isObject()) {
        appendWarning(outWarning, parser.error().empty() ? "root must be a JSON object" : parser.error());
        return false;
    }

    RadialConfig cfg = makeDefaultConfig();

    if (auto* f = root.find("version"); f && f->isNumber()) cfg.version = safeDoubleToInt(f->asNumber(), 1);
    if (auto* f = root.find("enabled"); f && f->isBool()) cfg.enabled = f->asBool();
    if (auto* f = root.find("language"); f && f->isString()) {
        if (auto l = parseUiLanguage(f->asString())) cfg.language = *l;
        else appendWarning(outWarning, "unknown language '" + f->asString() + "'");
    }
    if (auto* f = root.find("activationMode"); f && f->isString()) {
        if (auto m = parseActivationMode(f->asString())) cfg.activationMode = *m;
        else appendWarning(outWarning, "unknown activationMode '" + f->asString() + "'");
    }

    if (auto* f = root.find("activationKey")) {
        if (f->isString()) {
            if (auto vk = parseKeyName(f->asString())) cfg.activationKey = *vk;
            else appendWarning(outWarning, "unknown activationKey '" + f->asString() + "'");
        } else if (f->isNumber()) {
            cfg.activationKey = safeDoubleToInt(f->asNumber(), kVkMenu);
        }
    }
    if (auto* f = root.find("editorKey")) {
        if (f->isString()) {
            if (auto vk = parseKeyName(f->asString())) cfg.editorKey = *vk;
            else appendWarning(outWarning, "unknown editorKey '" + f->asString() + "'");
        } else if (f->isNumber()) {
            cfg.editorKey = safeDoubleToInt(f->asNumber(), kVkF12);
        }
    }

    if (auto* f = root.find("flickSensitivity"); f && f->isNumber()) cfg.flickSensitivity = static_cast<float>(f->asNumber());
    if (auto* f = root.find("outerRadius"); f && f->isNumber()) cfg.outerRadius = static_cast<float>(f->asNumber());
    if (auto* f = root.find("innerRadius"); f && f->isNumber()) cfg.innerRadius = static_cast<float>(f->asNumber());
    if (auto* f = root.find("deadzoneRadius"); f && f->isNumber()) cfg.deadzoneRadius = static_cast<float>(f->asNumber());
    if (auto* f = root.find("backgroundOpacity"); f && f->isNumber()) cfg.backgroundOpacity = static_cast<float>(f->asNumber());
    if (auto* f = root.find("fontScale"); f && f->isNumber()) cfg.fontScale = static_cast<float>(f->asNumber());
    if (auto* f = root.find("holdDelayMs"); f && f->isNumber()) cfg.holdDelayMs = safeDoubleToInt(f->asNumber(), 0);
    if (auto* f = root.find("submenuHoverMs"); f && f->isNumber()) cfg.submenuHoverMs = safeDoubleToInt(f->asNumber(), 320);
    if (auto* f = root.find("globalCooldownMs"); f && f->isNumber()) cfg.globalCooldownMs = safeDoubleToInt(f->asNumber(), 350);
    if (auto* f = root.find("executeOnRelease"); f && f->isBool()) cfg.executeOnRelease = f->asBool();
    if (auto* f = root.find("executeOnLeftClick"); f && f->isBool()) cfg.executeOnLeftClick = f->asBool();
    if (auto* f = root.find("autoSwitchPreset"); f && f->isBool()) cfg.autoSwitchPreset = f->asBool();
    if (auto* f = root.find("showPresetToast"); f && f->isBool()) cfg.showPresetToast = f->asBool();
    if (auto* f = root.find("showNumberHints"); f && f->isBool()) cfg.showNumberHints = f->asBool();
    if (auto* f = root.find("invertWheelScroll"); f && f->isBool()) cfg.invertWheelScroll = f->asBool();
    if (auto* f = root.find("verboseLog"); f && f->isBool()) cfg.verboseLog = f->asBool();
    if (auto* f = root.find("defaultPresetId"); f && f->isString()) cfg.defaultPresetId = f->asString();

    if (auto* f = root.find("presets"); f && f->isArray()) {
        std::vector<RadialPreset> parsedPresets;
        for (auto const& pv : f->asArray()) {
            if (!pv.isObject()) continue;
            RadialPreset p;
            if (auto* pf = pv.find("id"); pf && pf->isString()) p.id = pf->asString();
            if (auto* pf = pv.find("name"); pf && pf->isString()) p.name = pf->asString();
            if (auto* pf = pv.find("serverRules"); pf && pf->isArray()) {
                for (auto const& rv : pf->asArray()) {
                    if (!rv.isObject()) continue;
                    ServerRule r;
                    if (auto* rf = rv.find("hostPattern"); rf && rf->isString()) r.hostPattern = rf->asString();
                    if (auto* rf = rv.find("port"); rf && rf->isNumber()) r.port = safeDoubleToInt(rf->asNumber(), 0);
                    if (auto* rf = rv.find("serverNamePattern"); rf && rf->isString()) r.serverNamePattern = rf->asString();
                    if (auto* rf = rv.find("worldNamePattern"); rf && rf->isString()) r.worldNamePattern = rf->asString();
                    if (auto* rf = rv.find("scope"); rf && rf->isString()) {
                        if (auto sc = parseConnectionScope(rf->asString())) r.scope = *sc;
                    }
                    if (auto* rf = rv.find("priority"); rf && rf->isNumber()) {
                        r.priority = safeDoubleToInt(rf->asNumber(), 0);
                    }
                    p.serverRules.push_back(std::move(r));
                }
            }
            if (auto* pf = pv.find("slots"); pf && pf->isArray()) {
                for (auto const& sv : pf->asArray()) {
                    if (!sv.isObject()) continue;
                    RadialSlot slot;
                    parseSlotObject(sv, slot, 0, outWarning);
                    p.slots.push_back(std::move(slot));
                }
            }
            parsedPresets.push_back(std::move(p));
        }
        if (!parsedPresets.empty()) {
            cfg.presets = std::move(parsedPresets);
        } else {
            appendWarning(outWarning, "presets array empty; kept defaults");
        }
    }

    sanitizeConfig(cfg);
    outConfig = std::move(cfg);
    return true;
}

std::string serializeConfigJson(RadialConfig const& cfg) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::fixed << std::setprecision(2);
    os << "{\n";
    os << "  \"version\": " << cfg.version << ",\n";
    os << "  \"enabled\": " << (cfg.enabled ? "true" : "false") << ",\n";
    os << "  \"language\": " << jsonEscape(uiLanguageToString(cfg.language)) << ",\n";
    os << "  \"activationMode\": " << jsonEscape(activationModeToString(cfg.activationMode)) << ",\n";
    os << "  \"activationKey\": " << jsonEscape(keyCodeToName(cfg.activationKey)) << ",\n";
    os << "  \"editorKey\": " << jsonEscape(keyCodeToName(cfg.editorKey)) << ",\n";
    os << "  \"flickSensitivity\": " << safeFiniteFloat(cfg.flickSensitivity, 1.0f) << ",\n";
    os << "  \"outerRadius\": " << safeFiniteFloat(cfg.outerRadius, 92.0f) << ",\n";
    os << "  \"innerRadius\": " << safeFiniteFloat(cfg.innerRadius, 32.0f) << ",\n";
    os << "  \"deadzoneRadius\": " << safeFiniteFloat(cfg.deadzoneRadius, 22.0f) << ",\n";
    os << "  \"backgroundOpacity\": " << safeFiniteFloat(cfg.backgroundOpacity, 0.78f) << ",\n";
    os << "  \"fontScale\": " << safeFiniteFloat(cfg.fontScale, 0.95f) << ",\n";
    os << "  \"holdDelayMs\": " << cfg.holdDelayMs << ",\n";
    os << "  \"submenuHoverMs\": " << cfg.submenuHoverMs << ",\n";
    os << "  \"globalCooldownMs\": " << cfg.globalCooldownMs << ",\n";
    os << "  \"executeOnRelease\": " << (cfg.executeOnRelease ? "true" : "false") << ",\n";
    os << "  \"executeOnLeftClick\": " << (cfg.executeOnLeftClick ? "true" : "false") << ",\n";
    os << "  \"autoSwitchPreset\": " << (cfg.autoSwitchPreset ? "true" : "false") << ",\n";
    os << "  \"showPresetToast\": " << (cfg.showPresetToast ? "true" : "false") << ",\n";
    os << "  \"showNumberHints\": " << (cfg.showNumberHints ? "true" : "false") << ",\n";
    os << "  \"invertWheelScroll\": " << (cfg.invertWheelScroll ? "true" : "false") << ",\n";
    os << "  \"verboseLog\": " << (cfg.verboseLog ? "true" : "false") << ",\n";
    os << "  \"defaultPresetId\": " << jsonEscape(cfg.defaultPresetId) << ",\n";
    os << "  \"presets\": [\n";
    for (size_t i = 0; i < cfg.presets.size(); ++i) {
        auto const& p = cfg.presets[i];
        os << "    {\n";
        os << "      \"id\": " << jsonEscape(p.id) << ",\n";
        os << "      \"name\": " << jsonEscape(p.name) << ",\n";
        os << "      \"serverRules\": [";
        if (!p.serverRules.empty()) os << "\n";
        for (size_t r = 0; r < p.serverRules.size(); ++r) {
            auto const& rule = p.serverRules[r];
            os << "        { \"hostPattern\": " << jsonEscape(rule.hostPattern)
               << ", \"port\": " << rule.port
               << ", \"serverNamePattern\": " << jsonEscape(rule.serverNamePattern)
               << ", \"worldNamePattern\": " << jsonEscape(rule.worldNamePattern)
               << ", \"scope\": " << jsonEscape(connectionScopeToString(rule.scope))
               << ", \"priority\": " << rule.priority << " }";
            if (r + 1 < p.serverRules.size()) os << ",";
            os << "\n";
        }
        os << (p.serverRules.empty() ? "" : "      ") << "],\n";
        os << "      \"slots\": [\n";
        for (size_t s = 0; s < p.slots.size(); ++s) {
            serializeSlot(os, p.slots[s], 4);
            if (s + 1 < p.slots.size()) os << ",";
            os << "\n";
        }
        os << "      ]\n";
        os << "    }";
        if (i + 1 < cfg.presets.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    return os.str();
}

ConfigLoadResult loadConfigFile(std::filesystem::path const& path) {
    ConfigLoadResult res;
    res.config = makeDefaultConfig();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        res.existed = false;
        return res;
    }
    res.existed = true;

    auto fsize = std::filesystem::file_size(path, ec);
    if (!ec && fsize > kMaxConfigFileSize) {
        res.corrupted = true;
        res.warning   = "config file exceeds 2 MiB limit";
        return res;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        res.corrupted = true;
        res.warning   = "unable to open config file";
        return res;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    RadialConfig parsed;
    std::string  warn;
    if (!parseConfigJson(text, parsed, &warn)) {
        res.corrupted = true;
        res.warning   = warn.empty() ? "invalid JSON" : warn;
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()
        ).count();
        auto backup = path;
        backup += ".corrupt-" + std::to_string(ts);
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, ec);
        return res;
    }

    res.config  = std::move(parsed);
    res.warning = std::move(warn);
    return res;
}

bool saveConfigFile(std::filesystem::path const& path, RadialConfig const& cfg) {
    try {
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            std::string body = serializeConfigJson(cfg);
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
            out.flush();
            if (!out.good()) return false;
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::error_code copyEc;
            std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, copyEc);
            std::error_code rmEc;
            std::filesystem::remove(tmp, rmEc);
            if (copyEc) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

int findPresetIndex(RadialConfig const& cfg, std::string_view idOrName) noexcept {
    if (idOrName.empty()) return -1;
    std::string key = toLowerAscii(trimAscii(idOrName));
    for (size_t i = 0; i < cfg.presets.size(); ++i) {
        if (toLowerAscii(cfg.presets[i].id) == key) return static_cast<int>(i);
    }
    for (size_t i = 0; i < cfg.presets.size(); ++i) {
        if (toLowerAscii(cfg.presets[i].name) == key) return static_cast<int>(i);
    }
    return -1;
}

} // namespace radial
