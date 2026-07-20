#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "SubdivisionProbabilityGrid.h"

//==============================================================================
/** Step-17 editor: load button, reset-edits safety net, undo/redo, status
    label, loop-length/sensitivity controls (with a live preview while
    dragging sensitivity), fade controls, pitch mode (Repitch vs
    Time-Stretch, with its grain size/window shape controls), trigger mode
    (Slice Length vs Clock, with its clock-reference menu and subdivision
    probability grid), and the waveform display — which owns slice
    visualization, drag-and-drop loading, per-slice probability, manual
    slice add/move/remove, deleting auto-detected transients, a live
    playhead highlight, and modifier-key hover cues. */
class SlicerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Button::Listener,
                                    private juce::Timer
{
public:
    explicit SlicerAudioProcessorEditor (SlicerAudioProcessor&);
    ~SlicerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void buttonClicked (juce::Button* button) override;
    void timerCallback() override; // keeps Undo/Redo enabled-state in sync
    void chooseAndLoadFile();
    void updateAfterSampleOrSliceChange(); // refreshes status text, BPM display, and the waveform
    void updateTriggerModeVisibility(); // shows/hides the Clock-only controls
    void updatePitchModeVisibility(); // shows/hides the Time-Stretch-only controls

    SlicerAudioProcessor& processor;

    juce::TextButton loadButton { "Load Sample..." };
    juce::TextButton resetEditsButton { "Reset edits" };
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::Label statusLabel;

    juce::Label loopLengthLabel;
    juce::Slider loopLengthSlider; // integer bars, e.g. 1-8
    juce::Label calculatedBpmLabel;

    juce::Label pitchModeLabel;
    juce::ComboBox pitchModeSelector; // "Repitch" / "Time-Stretch"

    // Time-Stretch-only controls — same reserved-space/hide pattern as the
    // Clock-mode-only controls below (grain overlap is fixed at 50% and
    // deliberately not exposed here).
    juce::Label grainSizeLabel;
    juce::Slider grainSizeSlider;
    juce::Label windowShapeLabel;
    juce::ComboBox windowShapeSelector; // "Hann" / "Triangular"

    juce::Label sensitivityLabel;
    juce::Slider sensitivitySlider;

    juce::Label fadeInLabel;
    juce::Slider fadeInSlider;
    juce::Label fadeOutLabel;
    juce::Slider fadeOutSlider;

    juce::Label triggerModeLabel;
    juce::ComboBox triggerModeSelector; // "Slice Length" / "Clock"

    // Clock-mode-only controls — laid out in reserved space, hidden
    // (setVisible(false)) rather than the window resizing dynamically,
    // whenever Slice Length mode is selected.
    juce::Label clockReferenceLabel;
    juce::ComboBox clockReferenceSelector;

    juce::Label subdivisionTableLabel;
    SubdivisionProbabilityGrid subdivisionGrid;

    WaveformDisplay waveformDisplay;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
