#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"

//==============================================================================
/** Step-12 editor: load button, reset-edits safety net, undo/redo, status
    label, loop-length/sensitivity controls (with a live preview while
    dragging sensitivity), fade controls, and the waveform display — which
    owns slice visualization, drag-and-drop loading, per-slice probability,
    manual slice add/move/remove, deleting auto-detected transients, a
    live playhead highlight, and a Cmd-hover delete cue. */
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

    SlicerAudioProcessor& processor;

    juce::TextButton loadButton { "Load Sample..." };
    juce::TextButton resetEditsButton { "Reset edits" };
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::Label statusLabel;

    juce::Label loopLengthLabel;
    juce::Slider loopLengthSlider; // integer bars, e.g. 1-8
    juce::Label calculatedBpmLabel;

    juce::Label sensitivityLabel;
    juce::Slider sensitivitySlider;

    juce::Label fadeInLabel;
    juce::Slider fadeInSlider;
    juce::Label fadeOutLabel;
    juce::Slider fadeOutSlider;

    WaveformDisplay waveformDisplay;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
