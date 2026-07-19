#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/** Step-5 editor: load button, status label, generative-mode controls, and
    a slice selector + weight slider so individual slices can be made more
    or less likely to come up when generative mode substitutes. Waveform
    display and visible slice markers are still a later step — for now the
    slice selector is just "Slice 1", "Slice 2", etc. */
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
    void refreshSliceWeightControls(); // repopulates the combo box after a load

    SlicerAudioProcessor& processor;

    juce::TextButton loadButton { "Load Sample..." };
    juce::Label statusLabel;

    juce::ToggleButton generativeToggle { "Generative Mode" };
    juce::Slider probabilitySlider;
    juce::Label probabilitySliderLabel;

    juce::ComboBox sliceWeightSelector;
    juce::Slider sliceWeightSlider;
    juce::Label sliceWeightLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
