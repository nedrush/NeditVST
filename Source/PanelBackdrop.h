#pragma once

#include <JuceHeader.h>

//==============================================================================
/** A plain, untitled background card -- the darker (#1b1b1b-ish) wrapper
    behind the compact waveform toolbar+display, and behind a tab's own
    "remaining space" content area (e.g. Generate's revealed style
    parameters). Same "pure backdrop, doesn't own its children" convention
    as SectionPanel, just without a title bar -- these areas are visually
    self-explanatory (a waveform, a parameter panel) and don't need one. */
class PanelBackdrop : public juce::Component
{
public:
    PanelBackdrop()
    {
        setInterceptsMouseClicks (false, true); // pure backdrop -- clicks pass through to the real controls drawn on top
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.setColour (juce::Colour (0xFF1B1B1B));
        g.fillRoundedRectangle (bounds.toFloat(), 8.0f);

        g.setColour (juce::Colour (0xFF3A3A3A));
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 8.0f, 1.0f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanelBackdrop)
};
