#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Step-19: playback style probability (Forward / Ping-Pong), drawn as a
    horizontal multislider — same custom-painted pattern as
    SubdivisionProbabilityGrid (one row per option, label on the left, a
    draggable horizontal bar filling the rest of the row), but that class
    turned out to be wired directly to the subdivision table rather than
    built generically, so this is a small equivalent for the playback
    style table instead of a forced reuse.

    Drag anywhere in a row sets that style's probability from the
    horizontal position (left edge = 0.0, right edge = 1.0). Visible in
    both Trigger Modes — unlike the Clock-only subdivision grid, playback
    style is rolled on every pick in Slice Length mode too. */
class PlaybackStyleGrid : public juce::Component
{
public:
    explicit PlaybackStyleGrid (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;

    // Total height needed to show every row at once.
    static int getPreferredHeight();

private:
    void setProbabilityFromMouse (const juce::MouseEvent& event);
    int getRowIndexAtY (int y) const;

    SlicerAudioProcessor& processor;

    static constexpr int rowHeight = 18;
    static constexpr int labelWidth = 70;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackStyleGrid)
};
