#include "PluginProcessor.h"
#include "PluginEditor.h"

SlicerAudioProcessorEditor::SlicerAudioProcessorEditor (SlicerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), waveformDisplay (p)
{
    addAndMakeVisible (loadButton);
    loadButton.addListener (this);

    addAndMakeVisible (resetEditsButton);
    resetEditsButton.addListener (this);

    addAndMakeVisible (undoButton);
    undoButton.addListener (this);
    undoButton.setEnabled (false);

    addAndMakeVisible (redoButton);
    redoButton.addListener (this);
    redoButton.setEnabled (false);

    startTimerHz (10); // keeps Undo/Redo enabled-state in sync with the processor

    addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setText (processor.hasSample() ? processor.getLoadedFileName()
                                                : "No sample loaded",
                          juce::dontSendNotification);

    addAndMakeVisible (loopLengthLabel);
    loopLengthLabel.setText ("Loop length (bars)", juce::dontSendNotification);
    loopLengthLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (loopLengthSlider);
    loopLengthSlider.setSliderStyle (juce::Slider::IncDecButtons);
    loopLengthSlider.setRange (1.0, 8.0, 1.0);
    loopLengthSlider.setValue (processor.getLoopLengthBars(), juce::dontSendNotification);
    loopLengthSlider.onValueChange = [this]
    {
        processor.setLoopLengthBars ((int) loopLengthSlider.getValue());

        const double bpm = processor.getCalculatedOriginalBpm();
        calculatedBpmLabel.setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                     juce::dontSendNotification);
    };

    addAndMakeVisible (calculatedBpmLabel);
    calculatedBpmLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (sensitivityLabel);
    sensitivityLabel.setText ("Transient sensitivity", juce::dontSendNotification);
    sensitivityLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (sensitivitySlider);
    sensitivitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    sensitivitySlider.setRange (0.0, 1.0, 0.01);
    sensitivitySlider.setValue (processor.getSensitivity(), juce::dontSendNotification);
    sensitivitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    sensitivitySlider.onValueChange = [this]
    {
        // Committing (which resets every slice probability and briefly
        // restarts the chain) on EVERY value change would fire many times
        // a second while the user scrubs the slider — that's what was
        // causing the "rapid retriggering" glitch, and it was also
        // silently wiping any probability edits made on the waveform with
        // every pixel of movement. While actually dragging, just show a
        // live PREVIEW of where slices would land — no commit, no sound,
        // no lost edits — and only commit for real on release (onDragEnd)
        // or for non-drag changes (keyboard nudge, text box entry).
        if (sensitivitySlider.isMouseButtonDown())
        {
            auto preview = processor.previewSlicesAtSensitivity ((float) sensitivitySlider.getValue());
            waveformDisplay.showPreviewSlices (preview);
            return;
        }

        processor.setSensitivityAndRedetect ((float) sensitivitySlider.getValue());
        updateAfterSampleOrSliceChange();
    };
    sensitivitySlider.onDragEnd = [this]
    {
        processor.setSensitivityAndRedetect ((float) sensitivitySlider.getValue());
        updateAfterSampleOrSliceChange();
    };

    addAndMakeVisible (fadeInLabel);
    fadeInLabel.setText ("Fade in (ms)", juce::dontSendNotification);
    fadeInLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (fadeInSlider);
    fadeInSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeInSlider.setRange (0.0, 100.0, 0.5);
    fadeInSlider.setValue (processor.getFadeInMs(), juce::dontSendNotification);
    fadeInSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    fadeInSlider.onValueChange = [this]
    {
        processor.setFadeInMs ((float) fadeInSlider.getValue());
    };

    addAndMakeVisible (fadeOutLabel);
    fadeOutLabel.setText ("Fade out (ms)", juce::dontSendNotification);
    fadeOutLabel.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (fadeOutSlider);
    fadeOutSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeOutSlider.setRange (0.0, 100.0, 0.5);
    fadeOutSlider.setValue (processor.getFadeOutMs(), juce::dontSendNotification);
    fadeOutSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    fadeOutSlider.onValueChange = [this]
    {
        processor.setFadeOutMs ((float) fadeOutSlider.getValue());
    };

    addAndMakeVisible (waveformDisplay);
    waveformDisplay.onSampleChanged = [this] { updateAfterSampleOrSliceChange(); };

    setSize (560, 480);
}

SlicerAudioProcessorEditor::~SlicerAudioProcessorEditor()
{
    loadButton.removeListener (this);
    resetEditsButton.removeListener (this);
    undoButton.removeListener (this);
    redoButton.removeListener (this);
}

void SlicerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (14.0f);
    g.drawFittedText ("NeditVST — step 13: Shift bypasses snap for free placement",
                       getLocalBounds().removeFromTop (30), juce::Justification::centred, 1);
}

void SlicerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (30); // space for the paint() header text

    auto topButtonRow = area.removeFromTop (40);
    loadButton.setBounds (topButtonRow.removeFromLeft (topButtonRow.getWidth() - 110));
    topButtonRow.removeFromLeft (10);
    resetEditsButton.setBounds (topButtonRow);
    area.removeFromTop (10);
    statusLabel.setBounds (area.removeFromTop (30));
    area.removeFromTop (20);

    auto loopRow = area.removeFromTop (30);
    loopLengthLabel.setBounds (loopRow.removeFromLeft (140));
    loopLengthSlider.setBounds (loopRow.removeFromLeft (100));
    loopRow.removeFromLeft (10);
    calculatedBpmLabel.setBounds (loopRow);
    area.removeFromTop (20);

    auto sensitivityRow = area.removeFromTop (30);
    sensitivityLabel.setBounds (sensitivityRow.removeFromLeft (140));
    sensitivitySlider.setBounds (sensitivityRow);
    area.removeFromTop (20);

    auto fadeInRow = area.removeFromTop (30);
    fadeInLabel.setBounds (fadeInRow.removeFromLeft (140));
    fadeInSlider.setBounds (fadeInRow);
    area.removeFromTop (10);

    auto fadeOutRow = area.removeFromTop (30);
    fadeOutLabel.setBounds (fadeOutRow.removeFromLeft (140));
    fadeOutSlider.setBounds (fadeOutRow);
    area.removeFromTop (20);

    auto undoRedoRow = area.removeFromTop (30);
    undoButton.setBounds (undoRedoRow.removeFromLeft (100));
    undoRedoRow.removeFromLeft (10);
    redoButton.setBounds (undoRedoRow.removeFromLeft (100));
    area.removeFromTop (10);

    waveformDisplay.setBounds (area); // takes up all remaining space
}

void SlicerAudioProcessorEditor::buttonClicked (juce::Button* button)
{
    if (button == &loadButton)
        chooseAndLoadFile();
    else if (button == &resetEditsButton)
    {
        processor.resetAllManualEdits();
        updateAfterSampleOrSliceChange();
    }
    else if (button == &undoButton)
    {
        processor.undoLastEdit();
        updateAfterSampleOrSliceChange();
    }
    else if (button == &redoButton)
    {
        processor.redoLastEdit();
        updateAfterSampleOrSliceChange();
    }
}

void SlicerAudioProcessorEditor::timerCallback()
{
    undoButton.setEnabled (processor.canUndoEdit());
    redoButton.setEnabled (processor.canRedoEdit());
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
            updateAfterSampleOrSliceChange();
        }
    });
}

void SlicerAudioProcessorEditor::updateAfterSampleOrSliceChange()
{
    const int numSlices = processor.getNumSlices();
    const juce::String text = processor.getLoadedFileName()
                             + "  —  " + juce::String (numSlices)
                             + " slice" + (numSlices == 1 ? "" : "s");
    statusLabel.setText (text, juce::dontSendNotification);

    const double bpm = processor.getCalculatedOriginalBpm();
    calculatedBpmLabel.setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                 juce::dontSendNotification);

    waveformDisplay.refresh();
}
