#pragma once

#include "core/Localization.h"
#include "core/RadialConfig.h"
#include "core/RadialGeometry.h"
#include "core/ServerMatcher.h"

#include <string>
#include <string_view>
#include <vector>

namespace radial::editor {

enum class EditorTab {
    Wheel,
    Servers,
    Settings,
};

enum class TextFieldId {
    None,
    PresetName,
    HostPattern,
    ServerNamePattern,
    WorldNamePattern,
    RulePort,
    SlotLabel,
    SlotSubtitle,
    SlotValue,
    MacroStepValue,
};

enum class CaptureTarget {
    None,
    ActivationKey,
    EditorKey,
};

struct ColorSwatch {
    char const* name;
    char const* hex; // "" = auto
    uint32_t    rgb;
};

inline constexpr ColorSwatch kColorPalette[] = {
    // Row 1: Auto & Cool / Vibrant
    {"Auto",    "",        0x94A3B8},
    {"Sky",     "#38BDF8", 0x38BDF8},
    {"Cyan",    "#22D3EE", 0x22D3EE},
    {"Blue",    "#3B82F6", 0x3B82F6},
    {"Indigo",  "#6366F1", 0x6366F1},
    {"Violet",  "#A78BFA", 0xA78BFA},
    {"Fuchsia", "#E879F9", 0xE879F9},
    {"Pink",    "#F472B6", 0xF472B6},
    // Row 2: Warm / Nature / Neutral
    {"Red",     "#EF4444", 0xEF4444},
    {"Coral",   "#FB7185", 0xFB7185},
    {"Orange",  "#F97316", 0xF97316},
    {"Amber",   "#F59E0B", 0xF59E0B},
    {"Yellow",  "#FACC15", 0xFACC15},
    {"Lime",    "#A3E635", 0xA3E635},
    {"Emerald", "#34D399", 0x34D399},
    {"White",   "#F8FAFC", 0xF8FAFC},
};

struct EditorButton {
    geom::RectF rect;
    std::string id;
    std::string label;
    bool        active    = false;
    bool        accent    = false;
    bool        danger    = false;
    bool        success   = false;
    bool        disabled  = false;
    uint32_t    swatchRgb = 0; // non-zero for color swatch button
    bool        isBadge   = false; // non-interactive value/status display
    std::string hint;
};

struct EditorLabel {
    geom::RectF rect;
    std::string text;
    bool        accent     = false;
    bool        rightAlign = false;
};

struct EditorTextBox {
    geom::RectF rect;
    geom::RectF clearRect; // small [x] clear button on right edge when non-empty
    TextFieldId field        = TextFieldId::None;
    std::string caption;
    std::string value;
    std::string placeholder;
    bool        focused      = false;
    bool        selectedAll  = false;
    size_t      cursorByte   = 0;
    std::string hint;
};

struct EditorSlider {
    geom::RectF rect;
    std::string id;
    std::string label;
    std::string valueText;
    float       ratio = 0.0f; // 0..1
    std::string hint;
};

struct EditorToggle {
    geom::RectF rect;
    std::string id;
    std::string label;
    bool        checked = false;
    std::string hint;
};

struct WheelPreviewLayout {
    bool                    visible      = false;
    geom::RectF             bounds;
    float                   cx           = 0.0f;
    float                   cy           = 0.0f;
    float                   innerR       = 26.0f;
    float                   outerR       = 74.0f;
    float                   deadzoneR    = 18.0f;
    float                   bgOpacity    = 0.78f;
    float                   fontScale    = 0.95f;
    int                     selected     = -1;
    bool                    inSubmenu    = false;
    std::vector<RadialSlot> slots;
};

struct EditorLayout {
    ResolvedLang               lang                     = ResolvedLang::En;
    EditorTab                  activeTab                = EditorTab::Wheel;
    geom::RectF                panel;
    geom::RectF                header;
    geom::RectF                headerTextArea; // dynamically stops before header buttons
    geom::RectF                tabBar;
    geom::RectF                leftPane;
    geom::RectF                rightPane;
    bool                       confirmingClose          = false;
    geom::RectF                confirmBanner;
    WheelPreviewLayout         wheelPreview;
    std::vector<EditorLabel>   labels;
    std::vector<EditorButton>  buttons;
    std::vector<EditorTextBox> textBoxes;
    std::vector<EditorSlider>  sliders;
    std::vector<EditorToggle>  toggles;
    bool                       presetMatchesCurrent     = false;
    int                        selectedRuleIndex        = 0;
    int                        totalRules               = 0;
};

enum class EditorCommand {
    None,
    Save,
    SaveAndClose,
    Reload,
    Close,
    TestSlot,
};

// Pure C++20 state & layout model for the intuitive visual overlay editor (fully unit-tested).
class EditorModel {
public:
    void beginSession(
        RadialConfig const&   cfg,
        ServerIdentity const& server,
        int                   initialPresetIdx,
        bool                  systemIsJapanese = false
    );

    RadialConfig&       draft() noexcept { return mDraft; }
    RadialConfig const& draft() const noexcept { return mDraft; }
    bool                isDirty() const noexcept { return mDirty; }
    bool                canUndo() const noexcept { return !mUndoStack.empty(); }
    void                markSaved() noexcept { mDirty = false; mConfirmClose = false; }

    ResolvedLang            resolvedLang() const noexcept;
    EditorTab               activeTab() const noexcept { return mActiveTab; }
    int                     selectedPreset() const noexcept { return mSelectedPreset; }
    int                     selectedSlot() const noexcept { return mSelectedSlot; }
    int                     selectedMacroStep() const noexcept { return mSelectedMacroStep; }
    int                     selectedRule() const noexcept { return mSelectedRule; }
    TextFieldId             focusedField() const noexcept { return mFocusedField; }
    CaptureTarget           captureTarget() const noexcept { return mCaptureTarget; }
    std::string const&      statusMessage() const noexcept { return mStatus; }
    std::vector<int> const& submenuPath() const noexcept { return mSubmenuPath; }
    ServerIdentity const&   serverIdentity() const noexcept { return mServer; }
    bool                    isDraggingSlider() const noexcept { return !mDraggingSliderId.empty(); }

    void setStatus(std::string msg) { mStatus = std::move(msg); }

    EditorLayout buildLayout(float screenW, float screenH) const;

    // Hit-test interactive wheel preview (-1 = none, -2 = center hub, 0..N-1 = sector index).
    static int hitTestWheelPreview(WheelPreviewLayout const& wp, float mx, float my) noexcept;

    // Mouse interaction (click, click-to-caret, double-click select-all, continuous slider drag, wheel scroll).
    EditorCommand handleClick(
        float                     mx,
        float                     my,
        float                     screenW,
        float                     screenH,
        TextWidthMeasureFn const& inputMeasure  = {},
        bool                      isDoubleClick = false
    );
    bool          handleDrag(float mx, float my, float screenW, float screenH);
    void          endDrag() noexcept { mDraggingSliderId.clear(); }
    bool          handleMouseWheel(int delta);

    // Compute concise 1-line hover help text for the control under (mx, my), or empty if none.
    static std::string hoverHintAt(EditorLayout const& layout, float mx, float my);

    // Request close using the same unsaved-changes confirmation flow as the Close button.
    EditorCommand requestCloseWithConfirm();

    // Keyboard interaction (supports ToUnicode UTF-8 text, cursor movement, Tab cycling, Ctrl+A/C/V/Z/S, Delete).
    EditorCommand handleKey(
        int              vk,
        bool             shift,
        bool             ctrl,
        std::string_view clipboardUtf8 = {},
        std::string_view typedUtf8     = {},
        std::string*     outCopyUtf8   = nullptr
    );

    // Capture a key (including OS-polled Alt) or mouse side/middle button.
    bool handleCapturedKey(int vk);
    bool handleMouseButtonForCapture(int mouseVk);

    // Insert UTF-8 text at cursor (UTF-8 boundary safe).
    bool insertText(std::string_view utf8);

    // Accessors for currently edited containers.
    RadialPreset*                  currentPreset() noexcept;
    RadialPreset const*            currentPreset() const noexcept;
    std::vector<RadialSlot>*       currentSlotList() noexcept;
    std::vector<RadialSlot> const* currentSlotList() const noexcept;
    RadialSlot*                    currentSlot() noexcept;
    RadialSlot const*              currentSlot() const noexcept;
    ServerRule*                    currentRule() noexcept;
    ServerRule const*              currentRule() const noexcept;
    std::string                    currentBreadcrumb() const;

    bool createPresetForCurrentServer();
    bool undo();

private:
    std::string* focusedStringPtr() noexcept;
    std::string  getFieldValue(TextFieldId id) const;
    void         setFieldValue(TextFieldId id, std::string const& val);
    void         pushUndoSnapshot();
    void         ensureValidSelection() noexcept;
    void         ensureSelectedRule(RadialPreset& p);
    void         applySliderRatio(std::string_view id, float ratio, bool recordUndo);
    void         applyToggle(std::string_view id);
    void         focusField(TextFieldId id, bool moveCursorToEnd = true);
    void         cycleFocusField(bool backwards);
    static size_t prevUtf8Boundary(std::string const& s, size_t pos) noexcept;
    static size_t nextUtf8Boundary(std::string const& s, size_t pos) noexcept;

    static constexpr size_t kMaxUndoDepth = 32;

    bool                      mSystemIsJapanese = false;
    EditorTab                 mActiveTab        = EditorTab::Wheel;
    RadialConfig              mDraft;
    std::vector<RadialConfig> mUndoStack;
    ServerIdentity            mServer;
    int                       mSelectedPreset    = 0;
    std::vector<int>          mSubmenuPath;
    int                       mSelectedSlot      = 0;
    int                       mSelectedMacroStep = 0;
    int                       mMacroPage         = 0;
    int                       mSelectedRule      = 0;
    int                       mSettingsSlidePage = 0; // 0 or 1
    int                       mSettingsTogPage   = 0; // 0 or 1
    TextFieldId               mFocusedField      = TextFieldId::None;
    size_t                    mCursorByte        = 0;
    bool                      mSelectedAll       = false;
    std::string               mPortEditBuf;
    CaptureTarget             mCaptureTarget     = CaptureTarget::None;
    std::string               mDraggingSliderId;
    bool                      mDirty             = false;
    bool                      mConfirmClose      = false;
    bool                      mConfirmReset      = false;
    std::string               mStatus;
};

} // namespace radial::editor
