#include "SubdivisionProbabilityGrid.h"

SubdivisionProbabilityGrid::SubdivisionProbabilityGrid (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
}

int SubdivisionProbabilityGrid::getColumnIndexAtX (int x) const
{
    const int columnWidth = juce::jmax (1, getWidth() / SlicerAudioProcessor::numNoteValueOptions);
    return juce::jlimit (0, SlicerAudioProcessor::numNoteValueOptions - 1, x / columnWidth);
}

void SubdivisionProbabilityGrid::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    const int columnWidth = juce::jmax (1, bounds.getWidth() / SlicerAudioProcessor::numNoteValueOptions);
    const auto faderZone = bounds.withHeight (faderZoneHeight);

    g.setFont (9.0f);

    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
    {
        const juce::Rectangle<int> column (bounds.getX() + i * columnWidth, faderZone.getY(), columnWidth, faderZone.getHeight());
        const auto track = column.reduced ((columnWidth - faderWidth) / 2, 0).toFloat();

        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (track, 3.0f);

        const float probability = processor.getSubdivisionProbability (i);
        const auto filled = track.withY (track.getBottom() - track.getHeight() * probability)
                                  .withHeight (track.getHeight() * probability);

        g.setColour (juce::Colours::orange.withAlpha (0.75f));
        g.fillRoundedRectangle (filled, 3.0f);

        g.setColour (juce::Colours::orange.withAlpha (0.6f));
        g.drawRoundedRectangle (track, 3.0f, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.drawFittedText (SlicerAudioProcessor::getNoteValueName (i),
                           column.withY (faderZone.getBottom()).withHeight (labelHeight).reduced (1, 0),
                           juce::Justification::centred, 2, 0.7f);
    }
}

void SubdivisionProbabilityGrid::setProbabilityFromMouseY (int column, int y)
{
    const float fraction = 1.0f - juce::jlimit (0.0f, 1.0f, (float) y / (float) faderZoneHeight);
    processor.setSubdivisionProbability (column, fraction);
    repaint();
}

void SubdivisionProbabilityGrid::mouseDown (const juce::MouseEvent& event)
{
    setProbabilityFromMouseY (getColumnIndexAtX (event.x), event.y);
}

void SubdivisionProbabilityGrid::mouseDrag (const juce::MouseEvent& event)
{
    setProbabilityFromMouseY (getColumnIndexAtX (event.x), event.y);
}
