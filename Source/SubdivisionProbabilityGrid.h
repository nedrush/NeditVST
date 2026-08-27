#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Clock mode's subdivision probability control -- a row of compact
    vertical faders, one per note value, sharing the exact same visual/
    interaction language as PlaybackStyleFaderRow's playback-style
    probability row (fader height shows likelihood, top = 1.0, bottom = 0.0;
    clicking or dragging within a fader sets it), just applied to the 20-
    entry note-value palette (SlicerAudioProcessor::getNoteValueName()/
    numNoteValueOptions) instead of the 9 playback styles. Replaces the old
    20-row stack of full-width horizontal bars, which made the Timing
    section's Clock content the single largest driver of window height (see
    that section's own layout code in PluginEditor.cpp).

    Custom-painted and mouse-driven the same way PlaybackStyleFaderRow is:
    no juce::Slider objects, reads straight from the processor in paint()
    and writes straight back to it on click/drag. */
class SubdivisionProbabilityGrid : public juce::Component
{
public:
    explicit SubdivisionProbabilityGrid (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;

    // Fixed height, independent of the component's width (the note-value
    // count is fixed) -- same shape as PlaybackStyleFaderRow::getPreferredHeight().
    static int getPreferredHeight() { return faderZoneHeight + labelHeight; }

private:
    int getColumnIndexAtX (int x) const;
    void setProbabilityFromMouseY (int column, int y);

    SlicerAudioProcessor& processor;

    static constexpr int faderZoneHeight = 90;
    static constexpr int labelHeight = 16;
    static constexpr int faderWidth = 10;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubdivisionProbabilityGrid)
};
