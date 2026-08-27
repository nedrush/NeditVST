#include "PlaybackStyleParameterPanel.h"

namespace
{
    constexpr float rotaryStartAngle = juce::MathConstants<float>::pi * 1.2f;
    constexpr float rotaryEndAngle = juce::MathConstants<float>::pi * 2.8f;
}

PlaybackStyleParameterPanel::PlaybackStyleParameterPanel (SlicerAudioProcessor& processorToUse,
                                                            GetParameterValue getValueIn,
                                                            SetParameterValue setValueIn,
                                                            int initialStyleId,
                                                            std::function<void (int)> onStyleChangedIn)
    : processor (processorToUse),
      getValue (getValueIn ? std::move (getValueIn)
                            : GetParameterValue ([&processorToUse] (int paramIndex)
                                                  { return processorToUse.getSequencerCellParameterGlobalValue (paramIndex); })),
      setValue (setValueIn ? std::move (setValueIn)
                            : SetParameterValue ([&processorToUse] (int paramIndex, float value)
                                                  { processorToUse.setSequencerCellParameterGlobalValue (paramIndex, value); })),
      onStyleChanged (std::move (onStyleChangedIn))
{
    addAndMakeVisible (styleSelector);

    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
        styleSelector.addItem (SlicerAudioProcessor::getPlaybackStyleName (i), i + 1); // JUCE item IDs are 1-based

    styleSelector.setSelectedId (initialStyleId, juce::dontSendNotification);
    styleSelector.onChange = [this]
    {
        repaint();

        if (onStyleChanged)
            onStyleChanged (styleSelector.getSelectedId() - 1);
    };
}

std::vector<PlaybackStyleParameterPanel::DialColumn> PlaybackStyleParameterPanel::buildColumnsForStyle (int style)
{
    std::vector<DialColumn> columns;
    const auto applicable = SlicerAudioProcessor::getApplicableSequencerCellParameters (style);

    for (int paramIndex : applicable)
    {
        if (paramIndex == 5 || paramIndex == 19) // Subdivide, Volume -- excluded, see class doc comment
            continue;

        if (SlicerAudioProcessor::isSequencerCellParameterSwept (paramIndex))
            columns.push_back ({ paramIndex, paramIndex + 1, false });
        else if (SlicerAudioProcessor::isSequencerCellParameterDiscrete (paramIndex)
            && ! SlicerAudioProcessor::isSequencerCellParameterSteppedSlider (paramIndex))
            columns.push_back ({ paramIndex, -1, true });
        else
            columns.push_back ({ paramIndex, -1, false });
    }

    return columns;
}

void PlaybackStyleParameterPanel::setSelectedStyle (int styleIndex)
{
    const int newId = styleIndex + 1;

    if (styleSelector.getSelectedId() == newId)
        return;

    styleSelector.setSelectedId (newId, juce::dontSendNotification);
    repaint();
}

int PlaybackStyleParameterPanel::getPreferredHeight()
{
    return styleSelectorHeight + topGap + nameLabelHeight + labelGap + dialDiameter + labelGap + indicatorHeight;
}

PlaybackStyleParameterPanel::ColumnLayout PlaybackStyleParameterPanel::getColumnLayout (int columnIndex) const
{
    const int x = leftPadding + columnIndex * (columnWidth + columnGap);
    const int rowTop = styleSelectorHeight + topGap;

    ColumnLayout layout;
    layout.nameBounds = { x, rowTop, columnWidth, nameLabelHeight };

    const int dialTop = rowTop + nameLabelHeight + labelGap;
    layout.dialBounds = juce::Rectangle<int> (x, dialTop, columnWidth, dialDiameter)
                             .withSizeKeepingCentre (dialDiameter, dialDiameter);

    const int indicatorTop = dialTop + dialDiameter + labelGap;
    layout.indicatorBounds = { x, indicatorTop, columnWidth, indicatorHeight };

    return layout;
}

void PlaybackStyleParameterPanel::resized()
{
    styleSelector.setBounds (getLocalBounds().removeFromTop (styleSelectorHeight).removeFromLeft (200));
}

void PlaybackStyleParameterPanel::drawDial (juce::Graphics& g, juce::Rectangle<int> dialBounds, float t, bool active) const
{
    const auto bounds = dialBounds.toFloat();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f - 2.0f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);

    juce::Path backgroundArc;
    backgroundArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    g.strokePath (backgroundArc, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path valueArc;
    valueArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, rotaryStartAngle, angle, true);
    g.setColour (juce::Colours::cyan.withAlpha (active ? 1.0f : 0.85f));
    g.strokePath (valueArc, juce::PathStrokeType (active ? 4.0f : 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto knobBounds = bounds.reduced (radius * 0.3f);
    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.fillEllipse (knobBounds);
    g.setColour (juce::Colours::white.withAlpha (0.4f));
    g.drawEllipse (knobBounds, 1.0f);

    const float pointerLength = radius * 0.55f;
    juce::Path pointer;
    pointer.startNewSubPath (centre.x, centre.y);
    pointer.lineTo (centre.x + pointerLength * std::sin (angle), centre.y - pointerLength * std::cos (angle));
    g.setColour (juce::Colours::white);
    g.strokePath (pointer, juce::PathStrokeType (2.0f));
}

void PlaybackStyleParameterPanel::paint (juce::Graphics& g)
{
    const int style = styleSelector.getSelectedId() - 1;
    const auto columns = buildColumnsForStyle (style);

    if (columns.empty())
    {
        const auto emptyBounds = getColumnLayout (0).nameBounds.withRight (getWidth()).withHeight (dialDiameter);
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (12.0f);
        g.drawFittedText ("No adjustable parameters for this style", emptyBounds, juce::Justification::centredLeft, 1);
        return;
    }

    for (int i = 0; i < (int) columns.size(); ++i)
    {
        const auto& column = columns[(size_t) i];
        const auto layout = getColumnLayout (i);
        const juce::String name = SlicerAudioProcessor::getSequencerCellParameterName (column.paramIndex);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (10.0f);
        g.drawFittedText (name, layout.nameBounds, juce::Justification::centred, 2, 0.75f);

        float t = 0.0f;
        juce::String indicatorText;

        if (column.discreteSelect)
        {
            const int numOptions = SlicerAudioProcessor::getSequencerCellParameterNumOptions (column.paramIndex);
            const int currentOption = juce::roundToInt (getValue (column.paramIndex));
            t = numOptions > 1 ? (float) currentOption / (float) (numOptions - 1) : 0.0f;
            indicatorText = SlicerAudioProcessor::getSequencerCellParameterOptionName (column.paramIndex, currentOption);
        }
        else
        {
            const float value = getValue (column.paramIndex);
            const float minValue = SlicerAudioProcessor::getSequencerCellParameterMin (column.paramIndex);
            const float maxValue = SlicerAudioProcessor::getSequencerCellParameterMax (column.paramIndex);
            const float range = maxValue - minValue;
            t = range > 0.0f ? juce::jlimit (0.0f, 1.0f, (value - minValue) / range) : 0.0f;

            if (column.modeParamIndex >= 0)
            {
                const int modeOption = juce::roundToInt (getValue (column.modeParamIndex));
                indicatorText = SlicerAudioProcessor::getSequencerCellParameterOptionName (column.modeParamIndex, modeOption);
            }
            else
            {
                indicatorText = juce::String (value, 2);
            }
        }

        drawDial (g, layout.dialBounds, t, draggingParamIndex == column.paramIndex);

        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (9.5f);
        g.drawFittedText (indicatorText, layout.indicatorBounds, juce::Justification::centred, 1, 0.7f);
    }
}

void PlaybackStyleParameterPanel::showOptionsMenu (int paramIndex, juce::Rectangle<int> targetBounds)
{
    const int numOptions = SlicerAudioProcessor::getSequencerCellParameterNumOptions (paramIndex);
    const int currentOption = juce::roundToInt (getValue (paramIndex));

    juce::PopupMenu menu;

    for (int option = 0; option < numOptions; ++option)
        menu.addItem (option + 1, SlicerAudioProcessor::getSequencerCellParameterOptionName (paramIndex, option),
                      true, option == currentOption);

    // Parented to the top-level component, not this (potentially narrow)
    // panel, for the same reason SequencerGrid::showParameterMenuForCell()
    // does -- Rate's 20-item list would otherwise get squashed into
    // multiple columns by a parent too short to fit it as one list.
    const auto menuOptions = juce::PopupMenu::Options()
        .withParentComponent (getTopLevelComponent())
        .withTargetScreenArea (localAreaToGlobal (targetBounds));

    menu.showMenuAsync (menuOptions, [this, paramIndex] (int result)
    {
        if (result <= 0)
            return; // dismissed without choosing anything

        setValue (paramIndex, (float) (result - 1));
        repaint();
    });
}

void PlaybackStyleParameterPanel::mouseDown (const juce::MouseEvent& event)
{
    const int style = styleSelector.getSelectedId() - 1;
    const auto columns = buildColumnsForStyle (style);

    for (int i = 0; i < (int) columns.size(); ++i)
    {
        const auto layout = getColumnLayout (i);
        const auto columnBounds = layout.nameBounds.getUnion (layout.dialBounds).getUnion (layout.indicatorBounds);

        if (! columnBounds.contains (event.x, event.y))
            continue;

        const auto& column = columns[(size_t) i];

        if (column.discreteSelect)
        {
            showOptionsMenu (column.paramIndex, columnBounds);
        }
        else if (column.modeParamIndex >= 0 && layout.indicatorBounds.contains (event.x, event.y))
        {
            showOptionsMenu (column.modeParamIndex, layout.indicatorBounds);
        }
        else
        {
            draggingParamIndex = column.paramIndex;
            draggingStartY = event.y;
            draggingStartValue = getValue (column.paramIndex);
        }

        return;
    }
}

void PlaybackStyleParameterPanel::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingParamIndex < 0)
        return;

    constexpr float pixelsForFullRange = 150.0f;

    const float minValue = SlicerAudioProcessor::getSequencerCellParameterMin (draggingParamIndex);
    const float maxValue = SlicerAudioProcessor::getSequencerCellParameterMax (draggingParamIndex);
    const float deltaFraction = (float) (draggingStartY - event.y) / pixelsForFullRange;
    const float newValue = juce::jlimit (minValue, maxValue, draggingStartValue + deltaFraction * (maxValue - minValue));

    setValue (draggingParamIndex, newValue);
    repaint();
}

void PlaybackStyleParameterPanel::mouseUp (const juce::MouseEvent&)
{
    draggingParamIndex = -1;
    repaint();
}
