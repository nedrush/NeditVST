#include "PlaybackStyleGrid.h"

PlaybackStyleGrid::PlaybackStyleGrid (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
}

int PlaybackStyleGrid::getPreferredHeight()
{
    return SlicerAudioProcessor::numPlaybackStyleOptions * rowHeight;
}

void PlaybackStyleGrid::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    const float trackWidth = (float) juce::jmax (1, bounds.getWidth() - labelWidth);
    const float trackX = (float) (bounds.getX() + labelWidth);

    g.setFont (11.0f);

    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
    {
        const juce::Rectangle<int> row (bounds.getX(), bounds.getY() + i * rowHeight,
                                         bounds.getWidth(), rowHeight);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawText (SlicerAudioProcessor::getPlaybackStyleName (i),
                    row.withWidth (labelWidth - 6).withX (row.getX()),
                    juce::Justification::centredRight, false);

        const auto trackRow = row.reduced (0, 2).withX ((int) trackX).withWidth ((int) trackWidth);

        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (trackRow);

        const float probability = processor.getPlaybackStyleProbability (i);
        const float barWidth = trackWidth * probability;

        g.setColour (juce::Colours::orange.withAlpha (0.5f));
        g.fillRect (trackRow.toFloat().withWidth (barWidth));

        g.setColour (juce::Colours::orange.withAlpha (0.9f));
        g.drawRect (trackRow.toFloat(), 1.0f);
    }
}

int PlaybackStyleGrid::getRowIndexAtY (int y) const
{
    const int row = (y - getLocalBounds().getY()) / rowHeight;
    return juce::jlimit (0, SlicerAudioProcessor::numPlaybackStyleOptions - 1, row);
}

void PlaybackStyleGrid::setProbabilityFromMouse (const juce::MouseEvent& event)
{
    const int index = getRowIndexAtY (event.y);
    const float trackWidth = (float) juce::jmax (1, getWidth() - labelWidth);
    const float fraction = juce::jlimit (0.0f, 1.0f, (float) (event.x - labelWidth) / trackWidth);

    processor.setPlaybackStyleProbability (index, fraction);
    repaint();
}

void PlaybackStyleGrid::mouseDown (const juce::MouseEvent& event)
{
    setProbabilityFromMouse (event);
}

void PlaybackStyleGrid::mouseDrag (const juce::MouseEvent& event)
{
    setProbabilityFromMouse (event);
}
