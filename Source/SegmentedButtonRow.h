#pragma once

#include <JuceHeader.h>
#include <optional>

//==============================================================================
/** A small set of mutually-exclusive options, custom-painted (flat fill, no
    native ComboBox/TextButton chrome) with one option highlighted as
    selected -- the reusable building block behind Pitch Mode/Trigger Mode's
    segmented rows and the Playback Style selector on the Generate page, and
    also reused directly (not wrapped in a separate class) as the
    Generate/Sequence/Control/Perform sub-mode tab row in PluginEditor -- a
    tab bar is exactly this component's use case, so there's no separate
    TabBar class.

    Segments are laid out as equal-width slices of getLocalBounds() in
    resized(), separated by a small gap. Each option optionally carries its
    own colour (e.g. Playback Style reuses PlaybackStylePalette::getStyleColour())
    -- when given, the unselected state shows that colour at reduced alpha and
    the selected state shows it at full opacity with a highlighted border,
    the same "always show the swatch colour, vary border/alpha for selection"
    idea PlaybackStylePalette::paint() already uses. Options without a colour
    (Pitch Mode, Trigger Mode, both tab rows) fall back to the default
    Tungsten (unselected) / Salmon (selected) look. */
class SegmentedButtonRow : public juce::Component
{
public:
    struct Option
    {
        juce::String label;
        std::optional<juce::Colour> colour; // nullopt = default Tungsten/Salmon look
    };

    SegmentedButtonRow() = default;

    // Rebuilds the row from scratch. selectedIndex is clamped into range if
    // the new option list is shorter than the current selection.
    void setOptions (std::vector<Option> newOptions);

    // NotificationType matches juce::ComboBox::setSelectedId()'s own
    // convention -- dontSendNotification for programmatic/poll-and-resync
    // updates that must not re-enter processor state via onSelectionChanged.
    void setSelectedIndex (int index, juce::NotificationType notification = juce::sendNotification);
    int getSelectedIndex() const noexcept { return selectedIndex; }

    // Fires only when the selection actually changes as a result of a real
    // user click (never from setSelectedIndex(..., dontSendNotification),
    // and never re-fired for a click on the already-selected segment) --
    // matches juce::ComboBox::onChange's own "only on a real change" shape.
    std::function<void (int index)> onSelectionChanged;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override; // same "click sets state immediately" convention as PlaybackStyleGrid/PlaybackStylePalette elsewhere in this codebase

    static constexpr int preferredHeight = 30;

private:
    int getSegmentIndexAt (juce::Point<int> position) const;

    std::vector<Option> options;
    std::vector<juce::Rectangle<int>> segmentBounds; // cached in resized(), used for painting and hit-testing
    int selectedIndex = 0;

    static constexpr int segmentGap = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SegmentedButtonRow)
};
