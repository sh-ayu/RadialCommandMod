#pragma once

#include "core/RadialConfig.h"

#include <string>
#include <string_view>

namespace radial {

struct ServerIdentity {
    bool        inWorld    = false;
    bool        isLocal    = true;
    bool        isRealm    = false;
    std::string host;       // Unresolved domain / URL if available (e.g. "geo.hivebedrock.network")
    std::string ip;         // Resolved IP if available
    int         port       = 0;
    std::string serverName; // External server display name
    std::string worldName;  // Local world display name

    // Stable string fingerprint for detecting server/world transitions.
    std::string fingerprint() const;

    // Human-readable short summary (e.g. "geo.hivebedrock.network:19132" or "Local: My World").
    std::string displayLabel() const;

    bool operator==(ServerIdentity const& o) const noexcept = default;
};

struct RuleMatchScore {
    bool matched     = false;
    int  priority    = 0;
    int  specificity = 0;

    bool betterThan(RuleMatchScore const& o) const noexcept {
        if (!matched) return false;
        if (!o.matched) return true;
        if (priority != o.priority) return priority > o.priority;
        return specificity > o.specificity;
    }
};

struct PresetMatchResult {
    int            presetIndex = 0;
    bool           matchedRule = false;
    RuleMatchScore bestScore;
    std::string    matchedPresetId;
};

struct PlaceholderContext {
    std::string playerName;
    int         x = 0;
    int         y = 0;
    int         z = 0;
    std::string serverLabel;
    std::string worldName;
};

// Case-insensitive glob wildcard matcher supporting '*' (0..N chars) and '?' (1 char).
bool wildcardMatchI(std::string_view pattern, std::string_view text) noexcept;

// Check whether an IP string is a loopback address ("127.*", "::1", "localhost").
bool isLoopbackHost(std::string_view host) noexcept;

// Evaluate a single ServerRule against a ServerIdentity using lexicographic (priority, specificity).
RuleMatchScore evaluateServerRule(ServerRule const& rule, ServerIdentity const& server) noexcept;

// Evaluate whether ANY rule in a preset matches `server`.
RuleMatchScore evaluatePresetRules(RadialPreset const& preset, ServerIdentity const& server) noexcept;

// Select the highest-scoring preset for the current server, falling back to defaultPresetId.
PresetMatchResult matchPresetForServer(RadialConfig const& cfg, ServerIdentity const& server) noexcept;

// Generate a safe suggested host pattern (respecting IPv4/IPv6, Realms, and multi-label ccTLDs like co.jp/ne.jp).
std::string suggestHostPattern(ServerIdentity const& server);

// Expand dynamic placeholders: {player}, {x}, {y}, {z}, {server}, {world}.
std::string expandPlaceholders(std::string_view text, PlaceholderContext const& ctx);

} // namespace radial
