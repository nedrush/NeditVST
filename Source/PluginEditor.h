#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Step-4 editor: load button, status label, and the generative-mode
    controls — a toggle plus a probability slider for how often a random
    slice gets substituted for the mapped one. Waveform display and slice
    markers are still a later step. */
class SlicerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Button::Listener
{
public:
    explicit SlicerAudioProcessorEditor (SlicerAudioProcessor&);
    ~SlicerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void buttonClicked (juce::Button* button) override;
    void chooseAndLoadFile();

    SlicerAudioProcessor& processor;

    juce::TextButton loadButton { "Load Sample..." };
    juce::Label statusLabel;

    juce::ToggleButton generativeToggle { "Generative Mode" };
    juce::Slider probabilitySlider;
    juce::Label probabilitySliderLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
