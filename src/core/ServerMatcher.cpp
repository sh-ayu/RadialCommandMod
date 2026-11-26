#include "core/ServerMatcher.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <vector>

namespace radial {
namespace {

unsigned char lowerCh(char c) noexcept {
    return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string trimLower(std::string_view sv) {
    size_t b = 0;
    size_t e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) out.push_back(static_cast<char>(lowerCh(sv[i])));
    return out;
}

bool isLikelyIpAddress(std::string_view s) noexcept {
    if (s.empty()) return false;
    bool hasDot   = false;
    bool hasColon = false;
    for (char c : s) {
        if (c == '.') hasDot = true;
        else if (c == ':') hasColon = true;
        else if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    if (hasColon) return true; // IPv6
    if (hasDot) {
        for (char c : s) {
            if (c != '.' && !std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        return true; // IPv4
    }
    return false;
}

bool isMultiPartPublicSuffix(std::string_view suffix) noexcept {
    static constexpr std::string_view kSuffixes[] = {
        "co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp", "ed.jp",
        "co.uk", "org.uk", "ac.uk", "gov.uk",
        "com.au", "net.au", "org.au",
        "com.br", "com.cn", "com.tw", "co.kr", "com.sg",
    };
    for (auto s : kSuffixes) {
        if (suffix == s) return true;
    }
    return false;
}

bool hasWildcard(std::string_view pat) noexcept {
    return pat.find('*') != std::string_view::npos || pat.find('?') != std::string_view::npos;
}

} // namespace

NormalizedEndpoint normalizeHostEndpoint(std::string_view raw) {
    NormalizedEndpoint out;
    size_t b = 0;
    size_t e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    std::string_view s = raw.substr(b, e - b);
    if (s.empty()) return out;

    // Strip optional scheme prefix (e.g. "http://", "https://", "raknet://").
    if (size_t schemeSep = s.find("://"); schemeSep != std::string_view::npos && schemeSep > 0) {
        bool validScheme = std::isalpha(static_cast<unsigned char>(s[0])) != 0;
        for (size_t i = 1; i < schemeSep && validScheme; ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) {
                validScheme = false;
            }
        }
        if (validScheme) {
            s.remove_prefix(schemeSep + 3);
        }
    }

    // Strip path / query / fragment.
    if (size_t pathPos = s.find_first_of("/?#"); pathPos != std::string_view::npos) {
        s = s.substr(0, pathPos);
    }

    // Strip optional userinfo ("user@host").
    if (size_t atPos = s.rfind('@'); atPos != std::string_view::npos) {
        s.remove_prefix(atPos + 1);
    }

    auto parsePort = [](std::string_view psv) noexcept -> int {
        if (psv.empty() || psv.size() > 5) return 0;
        for (char c : psv) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
        }
        int  val = 0;
        auto res = std::from_chars(psv.data(), psv.data() + psv.size(), val);
        if (res.ec != std::errc{} || res.ptr != psv.data() + psv.size()) return 0;
        return (val >= 1 && val <= 65535) ? val : 0;
    };

    // Bracketed IPv6: "[::1]:19132" or "[::1]".
    if (!s.empty() && s.front() == '[') {
        if (size_t close = s.find(']'); close != std::string_view::npos) {
            out.host = std::string(s.substr(1, close - 1));
            if (close + 1 < s.size() && s[close + 1] == ':') {
                out.port = parsePort(s.substr(close + 2));
            }
            return out;
        }
    }

    // Split single ":port" suffix (never split unbracketed IPv6 with multiple colons).
    size_t const firstColon = s.find(':');
    if (firstColon != std::string_view::npos && s.find(':', firstColon + 1) == std::string_view::npos) {
        int const p = parsePort(s.substr(firstColon + 1));
        if (p > 0) {
            out.host = std::string(s.substr(0, firstColon));
            out.port = p;
            return out;
        }
    }

    out.host = std::string(s);
    return out;
}

std::string ServerIdentity::fingerprint() const {
    if (!inWorld) return "none";
    auto const normHost = normalizeHostEndpoint(host);
    auto const normIp   = normalizeHostEndpoint(ip);
    int const  effPort  = (port > 0) ? port : (normHost.port > 0 ? normHost.port : normIp.port);
    std::ostringstream os;
    os << (isLocal ? "local|" : (isRealm ? "realm|" : "ext|"))
       << trimLower(normHost.host) << "|"
       << trimLower(normIp.host) << "|"
       << effPort << "|"
       << trimLower(serverName) << "|"
       << trimLower(worldName);
    return os.str();
}

std::string ServerIdentity::displayLabel() const {
    if (!inWorld) return "Not in world";
    if (isLocal) {
        return worldName.empty() ? "Local World" : ("Local: " + worldName);
    }
    if (isRealm) {
        return !serverName.empty() ? ("Realm: " + serverName)
             : (!worldName.empty() ? ("Realm: " + worldName) : "Realm");
    }
    auto const  normHost = normalizeHostEndpoint(host);
    auto const  normIp   = normalizeHostEndpoint(ip);
    std::string addr     = !normHost.host.empty() ? normHost.host : normIp.host;
    int const   effPort  = (port > 0) ? port : (normHost.port > 0 ? normHost.port : normIp.port);
    if (!serverName.empty() && !addr.empty()) {
        return serverName + " (" + addr + ")";
    }
    if (!addr.empty()) {
        if (effPort > 0) {
            bool const isV6 = addr.find(':') != std::string::npos;
            return isV6 ? ("[" + addr + "]:" + std::to_string(effPort)) : (addr + ":" + std::to_string(effPort));
        }
        return addr;
    }
    if (!serverName.empty()) return serverName;
    return "External Server";
}

bool isLoopbackHost(std::string_view host) noexcept {
    try {
        std::string lo = trimLower(normalizeHostEndpoint(host).host);
        if (lo.empty()) return false;
        return lo == "localhost" || lo == "::1" || lo == "0.0.0.0" || lo.starts_with("127.");
    } catch (...) {
        return false;
    }
}

bool wildcardMatchI(std::string_view pattern, std::string_view text) noexcept {
    size_t pi = 0, ti = 0;
    size_t starIdx  = std::string_view::npos;
    size_t matchIdx = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || lowerCh(pattern[pi]) == lowerCh(text[ti]))) {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            starIdx  = pi++;
            matchIdx = ti;
        } else if (starIdx != std::string_view::npos) {
            pi = starIdx + 1;
            ti = ++matchIdx;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

RuleMatchScore evaluateServerRule(ServerRule const& rule, ServerIdentity const& server) noexcept {
    RuleMatchScore res;
    if (!server.inWorld) return res;

    if (rule.scope == ConnectionScope::External && server.isLocal) return res;
    if (rule.scope == ConnectionScope::Local && !server.isLocal) return res;

    int specificity = 0;
    if (rule.scope != ConnectionScope::Any) specificity += 10;

    if (!rule.hostPattern.empty()) {
        auto const normHost = normalizeHostEndpoint(server.host);
        auto const normIp   = normalizeHostEndpoint(server.ip);
        bool hostOk = (!normHost.host.empty() && wildcardMatchI(rule.hostPattern, normHost.host))
                   || (!server.host.empty() && wildcardMatchI(rule.hostPattern, server.host))
                   || (!normIp.host.empty() && wildcardMatchI(rule.hostPattern, normIp.host))
                   || (!server.ip.empty() && wildcardMatchI(rule.hostPattern, server.ip));
        if (!hostOk) return res;
        specificity += hasWildcard(rule.hostPattern) ? 40 : 60;
    }

    if (rule.port > 0) {
        int effPort = server.port;
        if (effPort <= 0) {
            auto const normHost = normalizeHostEndpoint(server.host);
            effPort             = (normHost.port > 0) ? normHost.port : normalizeHostEndpoint(server.ip).port;
        }
        if (effPort != rule.port) return res;
        specificity += 25;
    }

    if (!rule.serverNamePattern.empty()) {
        if (server.serverName.empty() || !wildcardMatchI(rule.serverNamePattern, server.serverName)) return res;
        specificity += hasWildcard(rule.serverNamePattern) ? 30 : 45;
    }

    if (!rule.worldNamePattern.empty()) {
        if (server.worldName.empty() || !wildcardMatchI(rule.worldNamePattern, server.worldName)) return res;
        specificity += hasWildcard(rule.worldNamePattern) ? 35 : 50;
    }

    // Reject completely empty "match anything" rules unless scope was explicitly narrowed.
    if (specificity == 0) return res;

    res.matched     = true;
    res.priority    = rule.priority;
    res.specificity = specificity;
    return res;
}

RuleMatchScore evaluatePresetRules(RadialPreset const& preset, ServerIdentity const& server) noexcept {
    RuleMatchScore best;
    for (auto const& rule : preset.serverRules) {
        auto sc = evaluateServerRule(rule, server);
        if (sc.betterThan(best)) best = sc;
    }
    return best;
}

PresetMatchResult matchPresetForServer(RadialConfig const& cfg, ServerIdentity const& server) noexcept {
    PresetMatchResult best;
    int defIdx = findPresetIndex(cfg, cfg.defaultPresetId);
    if (defIdx < 0) defIdx = 0;
    best.presetIndex     = cfg.presets.empty() ? -1 : defIdx;
    best.matchedPresetId = (best.presetIndex >= 0) ? cfg.presets[static_cast<size_t>(best.presetIndex)].id : "";

    if (!server.inWorld || cfg.presets.empty()) return best;

    RuleMatchScore bestScore;
    int            bestIdx = -1;
    for (size_t i = 0; i < cfg.presets.size(); ++i) {
        auto sc = evaluatePresetRules(cfg.presets[i], server);
        if (sc.betterThan(bestScore)) {
            bestScore = sc;
            bestIdx   = static_cast<int>(i);
        }
    }

    if (bestIdx >= 0) {
        best.presetIndex     = bestIdx;
        best.matchedRule     = true;
        best.bestScore       = bestScore;
        best.matchedPresetId = cfg.presets[static_cast<size_t>(bestIdx)].id;
    }
    return best;
}

std::string suggestHostPattern(ServerIdentity const& server) {
    // Realms use ephemeral IPs; prefer world/server name instead of volatile IP.
    if (server.isRealm && server.host.empty()) return "";

    std::string raw = !server.host.empty() ? normalizeHostEndpoint(server.host).host
                                           : normalizeHostEndpoint(server.ip).host;
    raw = trimLower(raw);
    if (raw.empty() || isLoopbackHost(raw)) return "";
    if (isLikelyIpAddress(raw)) return raw;

    // Split domain labels.
    std::vector<std::string_view> labels;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t dot = raw.find('.', start);
        if (dot == std::string::npos) {
            labels.push_back(std::string_view(raw).substr(start));
            break;
        }
        labels.push_back(std::string_view(raw).substr(start, dot - start));
        start = dot + 1;
    }

    if (labels.size() >= 3) {
        std::string lastTwo = std::string(labels[labels.size() - 2]) + "." + std::string(labels.back());
        // Keep full domain for 3-label multi-part ccTLD like "myserver.co.jp"; wildcard only for 4+ labels ("play.myserver.co.jp" -> "*.myserver.co.jp").
        if (isMultiPartPublicSuffix(lastTwo)) {
            if (labels.size() >= 4) {
                size_t firstDot = raw.find('.');
                return "*" + raw.substr(firstDot);
            }
            return raw;
        }
        size_t firstDot = raw.find('.');
        return "*" + raw.substr(firstDot);
    }
    return raw;
}

std::string expandPlaceholders(std::string_view text, PlaceholderContext const& ctx) {
    if (text.find('{') == std::string_view::npos) return std::string(text);

    std::string out;
    out.reserve(text.size() + 16);
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos && (close - i) <= 16) {
                std::string token = trimLower(text.substr(i + 1, close - i - 1));
                if (token == "player") {
                    out += ctx.playerName;
                    i = close + 1;
                    continue;
                }
                if (token == "x") {
                    out += std::to_string(ctx.x);
                    i = close + 1;
                    continue;
                }
                if (token == "y") {
                    out += std::to_string(ctx.y);
                    i = close + 1;
                    continue;
                }
                if (token == "z") {
                    out += std::to_string(ctx.z);
                    i = close + 1;
                    continue;
                }
                if (token == "server") {
                    out += ctx.serverLabel;
                    i = close + 1;
                    continue;
                }
                if (token == "world") {
                    out += ctx.worldName;
                    i = close + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

} // namespace radial
