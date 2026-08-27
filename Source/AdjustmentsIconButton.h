#pragma once

#include <JuceHeader.h>

//==============================================================================
/** A small hand-painted "adjustments" glyph button -- three stacked
    horizontal sliders, each with its own knob -- standing in for the
    Tabler-icon-font button the mockups use (not available in a native JUCE
    build). Opens the waveform panel's advanced-settings popover when
    clicked (see PluginEditor's showWaveformAdvancedPopover()). Deliberately
    tiny/muted -- context for the waveform toolbar, not a focal control. */
class AdjustmentsIconButton : public juce::Button
{
public:
    AdjustmentsIconButton() : juce::Button ("Advanced") { setTooltip ("More sample/detection settings"); }

    void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        const float alpha = isButtonDown ? 1.0f : (isMouseOverButton ? 0.85f : 0.65f);
        const juce::Colour lineColour = juce::Colours::white.withAlpha (alpha);
        const juce::Colour knobColour = juce::Colour (0xFFFF7E79).withAlpha (alpha); // Salmon

        static constexpr float knobPositions[3] = { 0.65f, 0.35f, 0.55f };
        const float rowGap = bounds.getHeight() / 3.0f;

        for (int i = 0; i < 3; ++i)
        {
            const float y = bounds.getY() + rowGap * (i + 0.5f);
            g.setColour (lineColour);
            g.drawLine (bounds.getX(), y, bounds.getRight(), y, 1.4f);

            const float knobX = bounds.getX() + bounds.getWidth() * knobPositions[i];
            g.setColour (knobColour);
            g.fillEllipse (knobX - 2.5f, y - 2.5f, 5.0f, 5.0f);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdjustmentsIconButton)
};
