#include "PlaybackStyleSwatchRow.h"
#include "PlaybackStylePalette.h"

PlaybackStyleSwatchRow::PlaybackStyleSwatchRow (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
    setSize (getPreferredWidth(), swatchSize);
}

int PlaybackStyleSwatchRow::getPreferredWidth()
{
    return SlicerAudioProcessor::numPlaybackStyleOptions * (swatchSize + swatchGap) - swatchGap;
}

int PlaybackStyleSwatchRow::getSwatchIndexAtX (int x) const
{
    const int column = x / (swatchSize + swatchGap);
    return juce::jlimit (0, SlicerAudioProcessor::numPlaybackStyleOptions - 1, column);
}

void PlaybackStyleSwatchRow::paint (juce::Graphics& g)
{
    const int selected = processor.getSelectedDrawingStyle();

    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
    {
        const juce::Rectangle<int> swatch (i * (swatchSize + swatchGap), 0, swatchSize, swatchSize);

        g.setColour (PlaybackStylePalette::getStyleColour (i));
        g.fillRoundedRectangle (swatch.toFloat(), 3.0f);

        const bool isSelected = (i == selected);
        g.setColour (isSelected ? juce::Colours::white : juce::Colours::black.withAlpha (0.45f));
        g.drawRoundedRectangle (swatch.toFloat().reduced (0.5f), 3.0f, isSelected ? 2.0f : 1.0f);

        if (processor.getRandomizeParametersForStyle (i))
        {
            g.setColour (juce::Colours::white);
            g.fillEllipse (swatch.getRight() - 6.0f, swatch.getY() + 1.0f, 4.0f, 4.0f);
        }
    }
}

void PlaybackStyleSwatchRow::mouseDown (const juce::MouseEvent& event)
{
    const int index = getSwatchIndexAtX (event.x);

    if (event.mods.isPopupMenu())
        processor.setRandomizeParametersForStyle (index, ! processor.getRandomizeParametersForStyle (index));
    else
        processor.setSelectedDrawingStyle (index);

    repaint();
}
