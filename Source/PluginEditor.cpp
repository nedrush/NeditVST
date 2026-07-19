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

    setSize (420, 220);
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
    g.drawFittedText ("NeditVST — step 3: slices mapped to MIDI notes",
                       getLocalBounds().removeFromTop (30), juce::Justification::centred, 1);
}

void SlicerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (30); // space for the paint() header text

    loadButton.setBounds (area.removeFromTop (40));
    area.removeFromTop (10);
    statusLabel.setBounds (area.removeFromTop (30));
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
