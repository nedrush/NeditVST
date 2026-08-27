#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Generate tab's playback-style probability control -- a row of compact
    vertical faders, one per PlaybackStyle, replacing the old horizontal
    probability-slider list (PlaybackStyleGrid). Fader height directly shows
    that style's likelihood (top = 1.0, bottom = 0.0), coloured per style via
    PlaybackStylePalette::getStyleColour() so it reads as the same palette
    used everywhere else a style needs a colour (Sequence's swatches,
    Control's keyboard washes, the sub-mode style-tab row).

    Clicking (or dragging within) a fader both sets that style's probability
    AND selects it -- onStyleSelected fires with the clicked style's index so
    a caller can drive an external "which style's parameters are showing"
    selector (PluginEditor's playbackStyleSegments) from either that row or
    directly from a fader, matching the mockup's "click its fader or a small
    tab above the row" -- both paths land on the same selection. */
class PlaybackStyleFaderRow : public juce::Component
{
public:
    explicit PlaybackStyleFaderRow (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;

    // Which style (if any) should be shown with a selection highlight --
    // purely a paint concern here; PluginEditor owns the real selection
    // state (playbackStyleSegments) and pushes it in.
    void setSelectedStyle (int styleIndex);

    std::function<void (int styleIndex)> onStyleSelected;

    static int getPreferredHeight() { return faderZoneHeight + labelHeight; }

private:
    int getColumnIndexAtX (int x) const;
    void setProbabilityFromMouseY (int column, int y);

    SlicerAudioProcessor& processor;
    int selectedStyle = -1;

    static constexpr int faderZoneHeight = 90;
    static constexpr int labelHeight = 16;
    static constexpr int faderWidth = 16;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackStyleFaderRow)
};
