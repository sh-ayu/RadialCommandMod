#include "core/ServerMatcher.h"

#include <algorithm>
#include <cctype>
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

std::string ServerIdentity::fingerprint() const {
    if (!inWorld) return "none";
    std::ostringstream os;
    os << (isLocal ? "local|" : (isRealm ? "realm|" : "ext|"))
       << trimLower(host) << "|"
       << trimLower(ip) << "|"
       << port << "|"
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
    std::string addr = !host.empty() ? host : ip;
    if (!serverName.empty() && !addr.empty()) {
        return serverName + " (" + addr + ")";
    }
    if (!addr.empty()) {
        return port > 0 ? (addr + ":" + std::to_string(port)) : addr;
    }
    if (!serverName.empty()) return serverName;
    return "External Server";
}

bool isLoopbackHost(std::string_view host) noexcept {
    std::string lo = trimLower(host);
    if (lo.empty()) return false;
    return lo == "localhost" || lo == "::1" || lo == "0.0.0.0" || lo.starts_with("127.");
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
        bool hostOk = (!server.host.empty() && wildcardMatchI(rule.hostPattern, server.host))
                   || (!server.ip.empty() && wildcardMatchI(rule.hostPattern, server.ip));
        if (!hostOk) return res;
        specificity += hasWildcard(rule.hostPattern) ? 40 : 60;
    }

    if (rule.port > 0) {
        if (server.port != rule.port) return res;
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

    std::string raw = !server.host.empty() ? server.host : server.ip;
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
