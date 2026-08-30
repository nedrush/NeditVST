#include "EffectsBusPanel.h"

namespace
{
    constexpr float rotaryStartAngle = juce::MathConstants<float>::pi * 1.2f;
    constexpr float rotaryEndAngle = juce::MathConstants<float>::pi * 2.8f;

    juce::String formatPercent (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }
}

EffectsBusPanel::EffectsBusPanel (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
    dials.push_back ({ "Time",
                        [this] { return processor.getDelayBusTimeMs(); },
                        [this] (float v) { processor.setDelayBusTimeMs (v); },
                        1.0f, 2000.0f,
                        [] (float v) { return juce::String (v, 0) + " ms"; } });

    dials.push_back ({ "Feedback",
                        [this] { return processor.getDelayBusFeedback(); },
                        [this] (float v) { processor.setDelayBusFeedback (v); },
                        0.0f, 0.95f,
                        formatPercent });

    dials.push_back ({ "Return",
                        [this] { return processor.getDelayBusReturnLevel(); },
                        [this] (float v) { processor.setDelayBusReturnLevel (v); },
                        0.0f, 1.0f,
                        formatPercent });

    dials.push_back ({ "Size",
                        [this] { return processor.getReverbBusSize(); },
                        [this] (float v) { processor.setReverbBusSize (v); },
                        0.0f, 1.0f,
                        formatPercent });

    dials.push_back ({ "Decay",
                        [this] { return processor.getReverbBusDecay(); },
                        [this] (float v) { processor.setReverbBusDecay (v); },
                        0.0f, 1.0f,
                        formatPercent });

    dials.push_back ({ "Return",
                        [this] { return processor.getReverbBusReturnLevel(); },
                        [this] (float v) { processor.setReverbBusReturnLevel (v); },
                        0.0f, 1.0f,
                        formatPercent });
}

int EffectsBusPanel::getPreferredHeight()
{
    return topPad + headerHeight + headerGap + nameLabelHeight + labelGap + dialDiameter + labelGap + indicatorHeight + bottomPad;
}

EffectsBusPanel::ColumnLayout EffectsBusPanel::getColumnLayout (int columnIndex) const
{
    const int extraGap = columnIndex >= 3 ? sectionGap : 0;
    const int x = leftPadding + columnIndex * (columnWidth + columnGap) + extraGap;
    const int nameTop = topPad + headerHeight + headerGap;

    ColumnLayout layout;
    layout.nameBounds = { x, nameTop, columnWidth, nameLabelHeight };

    const int dialTop = nameTop + nameLabelHeight + labelGap;
    layout.dialBounds = juce::Rectangle<int> (x, dialTop, columnWidth, dialDiameter)
                             .withSizeKeepingCentre (dialDiameter, dialDiameter);

    const int indicatorTop = dialTop + dialDiameter + labelGap;
    layout.indicatorBounds = { x, indicatorTop, columnWidth, indicatorHeight };

    return layout;
}

void EffectsBusPanel::drawDial (juce::Graphics& g, juce::Rectangle<int> dialBounds, float t, bool active) const
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

void EffectsBusPanel::paint (juce::Graphics& g)
{
    const auto delayLeft = getColumnLayout (0).nameBounds.getX();
    const auto delayRight = getColumnLayout (2).nameBounds.getRight();
    const auto reverbLeft = getColumnLayout (3).nameBounds.getX();
    const auto reverbRight = getColumnLayout (5).nameBounds.getRight();

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    g.drawFittedText ("DELAY", { delayLeft, topPad, delayRight - delayLeft, headerHeight }, juce::Justification::centred, 1);
    g.drawFittedText ("REVERB", { reverbLeft, topPad, reverbRight - reverbLeft, headerHeight }, juce::Justification::centred, 1);

    for (int i = 0; i < (int) dials.size(); ++i)
    {
        const auto& dial = dials[(size_t) i];
        const auto layout = getColumnLayout (i);

        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (10.0f);
        g.drawFittedText (dial.name, layout.nameBounds, juce::Justification::centred, 1, 0.75f);

        const float value = dial.getValue();
        const float range = dial.maxValue - dial.minValue;
        const float t = range > 0.0f ? juce::jlimit (0.0f, 1.0f, (value - dial.minValue) / range) : 0.0f;

        drawDial (g, layout.dialBounds, t, draggingColumn == i);

        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.setFont (9.5f);
        g.drawFittedText (dial.formatValue (value), layout.indicatorBounds, juce::Justification::centred, 1, 0.7f);
    }
}

int EffectsBusPanel::hitTestColumn (juce::Point<int> position) const
{
    for (int i = 0; i < (int) dials.size(); ++i)
    {
        const auto layout = getColumnLayout (i);
        const auto columnBounds = layout.nameBounds.getUnion (layout.dialBounds).getUnion (layout.indicatorBounds);

        if (columnBounds.contains (position))
            return i;
    }

    return -1;
}

void EffectsBusPanel::mouseDown (const juce::MouseEvent& event)
{
    const int column = hitTestColumn (event.getPosition());

    if (column < 0)
        return;

    draggingColumn = column;
    draggingStartY = event.y;
    draggingStartValue = dials[(size_t) column].getValue();
}

void EffectsBusPanel::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingColumn < 0)
        return;

    constexpr float pixelsForFullRange = 150.0f;

    const auto& dial = dials[(size_t) draggingColumn];
    const float deltaFraction = (float) (draggingStartY - event.y) / pixelsForFullRange;
    const float newValue = juce::jlimit (dial.minValue, dial.maxValue,
                                          draggingStartValue + deltaFraction * (dial.maxValue - dial.minValue));

    dial.setValue (newValue);
    repaint();
}

void EffectsBusPanel::mouseUp (const juce::MouseEvent&)
{
    draggingColumn = -1;
    repaint();
}
