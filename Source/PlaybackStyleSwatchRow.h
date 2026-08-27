#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Sequence tab's compact style palette -- one small colour swatch per
    PlaybackStyle, laid out in a single horizontal row (replacing
    PlaybackStylePalette's taller vertical, checkbox-per-row sidebar) so the
    sequencer grid beside/below it keeps the large majority of the tab's
    space. Left-click a swatch to set it as the processor's currently
    selected drawing style (same SlicerAudioProcessor::setSelectedDrawingStyle()
    SequencerGrid's own mouse handling already reads); the selected swatch
    gets a highlighted border, same convention as PlaybackStylePalette's.

    PlaybackStylePalette's per-style "randomize parameters too" checkbox
    (getRandomizeParametersForStyle()/setRandomizeParametersForStyle()) has
    no room left at this width, so it moves to right-click instead of a
    visible tickbox -- right-clicking a swatch toggles that same flag, shown
    as a small dot in the swatch's corner rather than a full checkbox. */
class PlaybackStyleSwatchRow : public juce::Component
{
public:
    explicit PlaybackStyleSwatchRow (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& event) override;

    static int getPreferredWidth();
    static constexpr int swatchSize = 16;

private:
    int getSwatchIndexAtX (int x) const;

    SlicerAudioProcessor& processor;

    static constexpr int swatchGap = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackStyleSwatchRow)
};
