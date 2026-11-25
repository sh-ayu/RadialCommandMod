#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace radial {

enum class UiLanguage {
    Auto,
    Ja,
    En,
};

enum class ResolvedLang {
    Ja,
    En,
};

std::string_view uiLanguageToString(UiLanguage lang) noexcept;
std::optional<UiLanguage> parseUiLanguage(std::string_view sv) noexcept;
ResolvedLang resolveUiLanguage(UiLanguage pref, bool systemIsJapanese) noexcept;

// Translate a UI text key into Japanese or English.
char const* tr(ResolvedLang lang, std::string_view key) noexcept;

// --- UTF-8 & text display width helpers ---

// Truncate UTF-8 string to at most `maxBytes` without ever splitting a multi-byte codepoint.
std::string truncateUtf8Bytes(std::string_view sv, size_t maxBytes);

// Truncate UTF-8 string to at most `maxCodepoints` (appending ".." if truncated).
std::string truncateUtf8Codepoints(std::string_view sv, size_t maxCodepoints);

// Estimate display width in GUI pixels (CJK/fullwidth = 2.0 units, ASCII = 1.0 unit; strips § color codes).
float estimateDisplayWidth(std::string_view utf8, float fontScale) noexcept;

using TextWidthMeasureFn = std::function<float(std::string_view)>;

// Fit UTF-8 text into `maxWidthPx` using an exact font measurer (falling back to estimateDisplayWidth).
// Appends ".." when truncated and guarantees valid UTF-8.
std::string fitTextToWidth(
    std::string_view          utf8,
    float                     maxWidthPx,
    float                     fontScale,
    TextWidthMeasureFn const& measure = {}
);

struct CaretTextSlice {
    std::string visibleText;
    float       caretOffsetPx = 0.0f;
};

// Compute a UTF-8 safe horizontal viewport slice around `cursorByte` that fits inside `maxWidthPx`,
// plus the exact pixel offset of the caret from the left edge of `visibleText`.
CaretTextSlice sliceTextForCaret(
    std::string_view          utf8,
    size_t                    cursorByte,
    float                     maxWidthPx,
    float                     fontScale,
    TextWidthMeasureFn const& measure = {}
);

// Compute the nearest UTF-8 byte boundary for a mouse click at `clickOffsetPx` inside a text box.
size_t caretByteFromClickOffset(
    std::string_view          utf8,
    size_t                    prevCursorByte,
    float                     clickOffsetPx,
    float                     maxWidthPx,
    float                     fontScale,
    TextWidthMeasureFn const& measure = {}
);

} // namespace radial
