#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Step-16: subdivision probability, drawn as a horizontal multislider —
    one row per note value (label on the left, a draggable horizontal bar
    filling the rest of the row). Replaces the old vertical-slider strip
    that scrolled horizontally, so every note value is visible at once.

    Custom-painted and mouse-driven the same way WaveformDisplay handles
    its own per-slice probability faders: no juce::Slider objects, the
    component reads straight from the processor in paint() and writes
    straight back to it on drag.

    Drag anywhere in a row sets that note value's probability from the
    horizontal position (left edge = 0.0, right edge = 1.0). */
class SubdivisionProbabilityGrid : public juce::Component
{
public:
    explicit SubdivisionProbabilityGrid (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;

    // Total height needed to show every row at once, given the component's
    // current width (the row count is fixed, so this doesn't depend on it,
    // but matches the getPreferredHeight()-style helper WaveformDisplay's
    // caller would use for layout).
    static int getPreferredHeight();

private:
    void setProbabilityFromMouse (const juce::MouseEvent& event);
    int getRowIndexAtY (int y) const;

    SlicerAudioProcessor& processor;

    static constexpr int rowHeight = 16;
    static constexpr int labelWidth = 44;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubdivisionProbabilityGrid)
};
