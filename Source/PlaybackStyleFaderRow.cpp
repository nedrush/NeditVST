#include "PlaybackStyleFaderRow.h"
#include "PlaybackStylePalette.h"

PlaybackStyleFaderRow::PlaybackStyleFaderRow (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
}

void PlaybackStyleFaderRow::setSelectedStyle (int styleIndex)
{
    if (selectedStyle == styleIndex)
        return;

    selectedStyle = styleIndex;
    repaint();
}

int PlaybackStyleFaderRow::getColumnIndexAtX (int x) const
{
    const int columnWidth = juce::jmax (1, getWidth() / SlicerAudioProcessor::numPlaybackStyleOptions);
    return juce::jlimit (0, SlicerAudioProcessor::numPlaybackStyleOptions - 1, x / columnWidth);
}

void PlaybackStyleFaderRow::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    const int columnWidth = juce::jmax (1, bounds.getWidth() / SlicerAudioProcessor::numPlaybackStyleOptions);
    const auto faderZone = bounds.withHeight (faderZoneHeight);

    g.setFont (9.0f);

    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
    {
        const auto colour = PlaybackStylePalette::getStyleColour (i);
        const juce::Rectangle<int> column (bounds.getX() + i * columnWidth, faderZone.getY(), columnWidth, faderZone.getHeight());
        const auto track = column.reduced ((columnWidth - faderWidth) / 2, 0).toFloat();

        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (track, 3.0f);

        const float probability = processor.getPlaybackStyleProbability (i);
        const auto filled = track.withY (track.getBottom() - track.getHeight() * probability)
                                  .withHeight (track.getHeight() * probability);

        const bool selected = (i == selectedStyle);
        g.setColour (colour.withAlpha (selected ? 0.95f : 0.75f));
        g.fillRoundedRectangle (filled, 3.0f);

        g.setColour (selected ? juce::Colours::white : colour.withAlpha (0.6f));
        g.drawRoundedRectangle (track, 3.0f, selected ? 2.0f : 1.0f);

        g.setColour (juce::Colours::white.withAlpha (selected ? 0.9f : 0.6f));
        g.drawFittedText (SlicerAudioProcessor::getPlaybackStyleName (i),
                           column.withY (faderZone.getBottom()).withHeight (labelHeight).reduced (1, 0),
                           juce::Justification::centred, 2, 0.7f);
    }
}

void PlaybackStyleFaderRow::setProbabilityFromMouseY (int column, int y)
{
    const float fraction = 1.0f - juce::jlimit (0.0f, 1.0f, (float) y / (float) faderZoneHeight);
    processor.setPlaybackStyleProbability (column, fraction);
    repaint();
}

void PlaybackStyleFaderRow::mouseDown (const juce::MouseEvent& event)
{
    const int column = getColumnIndexAtX (event.x);

    setSelectedStyle (column);
    if (onStyleSelected)
        onStyleSelected (column);

    setProbabilityFromMouseY (column, event.y);
}

void PlaybackStyleFaderRow::mouseDrag (const juce::MouseEvent& event)
{
    setProbabilityFromMouseY (getColumnIndexAtX (event.x), event.y);
}
