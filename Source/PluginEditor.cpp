#include "PluginProcessor.h"
#include "PluginEditor.h"

SlicerAudioProcessorEditor::SlicerAudioProcessorEditor (SlicerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    addAndMakeVisible (loadButton);
    loadButton.addListener (this);

    addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setText (processor.hasSample() ? processor.getLoadedFileName()
                                                : "No sample loaded",
                          juce::dontSendNotification);

    addAndMakeVisible (generativeToggle);
    generativeToggle.setToggleState (processor.generativeModeEnabled.load(),
                                      juce::dontSendNotification);
    generativeToggle.onClick = [this]
    {
        processor.generativeModeEnabled.store (generativeToggle.getToggleState());
    };

    addAndMakeVisible (probabilitySliderLabel);
    probabilitySliderLabel.setText ("Random slice chance", juce::dontSendNotification);
    probabilitySliderLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (probabilitySlider);
    probabilitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    probabilitySlider.setRange (0.0, 1.0, 0.01);
    probabilitySlider.setValue (processor.randomSliceProbability.load(), juce::dontSendNotification);
    probabilitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    probabilitySlider.onValueChange = [this]
    {
        processor.randomSliceProbability.store ((float) probabilitySlider.getValue());
    };

    setSize (420, 300);
}

SlicerAudioProcessorEditor::~SlicerAudioProcessorEditor()
{
    loadButton.removeListener (this);
}

void SlicerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (14.0f);
    g.drawFittedText ("NeditVST — step 4: basic generative slice triggering",
                       getLocalBounds().removeFromTop (30), juce::Justification::centred, 1);
}

void SlicerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (30); // space for the paint() header text

    loadButton.setBounds (area.removeFromTop (40));
    area.removeFromTop (10);
    statusLabel.setBounds (area.removeFromTop (30));
    area.removeFromTop (20);

    generativeToggle.setBounds (area.removeFromTop (30));
    area.removeFromTop (10);

    auto sliderRow = area.removeFromTop (30);
    probabilitySliderLabel.setBounds (sliderRow.removeFromLeft (140));
    probabilitySlider.setBounds (sliderRow);
}

void SlicerAudioProcessorEditor::buttonClicked (juce::Button* button)
{
    if (button == &loadButton)
        chooseAndLoadFile();
}

void SlicerAudioProcessorEditor::chooseAndLoadFile()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Select an audio sample to load...",
        juce::File(),
        "*.wav;*.aif;*.aiff;*.flac");

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file.existsAsFile())
        {
            processor.loadSample (file);

            const int numSlices = processor.getNumSlices();
            const int rootNote = processor.getRootNote();
            const int topNote = rootNote + juce::jmax (0, numSlices - 1);

            const juce::String rootName = juce::MidiMessage::getMidiNoteName (rootNote, true, true, 3);
            const juce::String topName = juce::MidiMessage::getMidiNoteName (topNote, true, true, 3);

            const juce::String text = processor.getLoadedFileName()
                                     + "  —  " + juce::String (numSlices)
                                     + " slice" + (numSlices == 1 ? "" : "s")
                                     + "  (" + rootName + " to " + topName + ")";
            statusLabel.setText (text, juce::dontSendNotification);
        }
    });
}
