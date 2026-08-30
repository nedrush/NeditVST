#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Persistent Delay/Reverb send-bus control panel (Pass 1) -- a fixed-
    footprint row of six compact rotary dials, always visible regardless of
    which top-level tab/mode is active (added to controlsContent, not
    subModeContent, by the editor -- same persistence trick
    SlicerAudioProcessorEditor already uses for waveformDisplay). Three
    dials control the Delay bus (Time/Feedback/Return), three the Reverb bus
    (Size/Decay/Return) -- both buses run continuously, independent of any
    pick's lifecycle (see SlicerAudioProcessor::processSendBuses()).

    Visually and interactively modelled on PlaybackStyleParameterPanel's own
    dials (same arc-plus-knob-plus-pointer paint, same vertical-drag-to-
    adjust interaction), but not built on that class -- this panel's six
    parameters are a fixed flat list, not a per-playback-style, per-index
    lookup, so there's no shared style-selection/discrete-option machinery
    worth reusing here. */
class EffectsBusPanel : public juce::Component
{
public:
    explicit EffectsBusPanel (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;
    void resized() override {} // fixed dial layout, computed purely from getLocalBounds() in getColumnLayout() -- nothing to lay out here

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // Fixed height for the "DELAY"/"REVERB" header row plus exactly one row
    // of dials -- never varies, so callers can reserve this once (same
    // "fixed-footprint container" convention as
    // PlaybackStyleParameterPanel::getPreferredHeight()).
    static int getPreferredHeight();

private:
    struct Dial
    {
        juce::String name;
        std::function<float()> getValue;
        std::function<void (float)> setValue;
        float minValue, maxValue;
        std::function<juce::String (float)> formatValue;
    };

    struct ColumnLayout
    {
        juce::Rectangle<int> nameBounds, dialBounds, indicatorBounds;
    };

    ColumnLayout getColumnLayout (int columnIndex) const;
    void drawDial (juce::Graphics& g, juce::Rectangle<int> dialBounds, float t, bool active) const;
    int hitTestColumn (juce::Point<int> position) const;

    SlicerAudioProcessor& processor;
    std::vector<Dial> dials; // 0-2: Delay Time/Feedback/Return, 3-5: Reverb Size/Decay/Return

    static constexpr int topPad = 8;
    static constexpr int headerHeight = 12;
    static constexpr int headerGap = 4;
    static constexpr int nameLabelHeight = 12;
    static constexpr int labelGap = 3;
    static constexpr int dialDiameter = 42;
    static constexpr int indicatorHeight = 12;
    static constexpr int bottomPad = 8;
    static constexpr int columnWidth = 72;
    static constexpr int columnGap = 8;
    static constexpr int sectionGap = 16; // extra gap between the Delay and Reverb dial groups (before column index 3)
    static constexpr int leftPadding = 4;

    // Vertical-drag rotary state -- -1 when no dial is being dragged. Same
    // "drag up increases, drag down decreases" convention as
    // PlaybackStyleParameterPanel.
    int draggingColumn = -1;
    int draggingStartY = 0;
    float draggingStartValue = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectsBusPanel)
};
