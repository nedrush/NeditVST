#include "PluginProcessor.h"
#include "PluginEditor.h"

SlicerAudioProcessorEditor::SlicerAudioProcessorEditor (SlicerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), waveformDisplay (p), subdivisionGrid (p), playbackStyleGrid (p)
{
    addAndMakeVisible (controlsViewport);
    controlsViewport.setViewedComponent (&controlsContent, false); // we own it, don't let the viewport delete it
    controlsViewport.setScrollBarsShown (true, false); // vertical only, shown when needed

    controlsContent.addAndMakeVisible (loadButton);
    loadButton.addListener (this);

    controlsContent.addAndMakeVisible (resetEditsButton);
    resetEditsButton.addListener (this);

    controlsContent.addAndMakeVisible (undoButton);
    undoButton.addListener (this);
    undoButton.setEnabled (false);

    controlsContent.addAndMakeVisible (redoButton);
    redoButton.addListener (this);
    redoButton.setEnabled (false);

    controlsContent.addAndMakeVisible (auditionButton);
    auditionButton.addListener (this);

    startTimerHz (10); // keeps Undo/Redo enabled-state (and the Audition button's label/colour) in sync with the processor

    controlsContent.addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setText (processor.hasSample() ? processor.getLoadedFileName()
                                                : "No sample loaded",
                          juce::dontSendNotification);

    controlsContent.addAndMakeVisible (loopLengthLabel);
    loopLengthLabel.setText ("Loop length (bars)", juce::dontSendNotification);
    loopLengthLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (loopLengthSlider);
    loopLengthSlider.setSliderStyle (juce::Slider::IncDecButtons);
    loopLengthSlider.setRange (1.0, 8.0, 1.0);
    loopLengthSlider.setValue (processor.getLoopLengthBars(), juce::dontSendNotification);
    loopLengthSlider.onValueChange = [this]
    {
        processor.setLoopLengthBars ((int) loopLengthSlider.getValue());

        const double bpm = processor.getCalculatedOriginalBpm();
        calculatedBpmLabel.setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                     juce::dontSendNotification);

        // Any interaction with this control counts as acknowledgment
        // (Step 33) -- even re-entering the same value -- so the
        // staleness highlight clears the moment the user looks at it,
        // not only when the value actually changes.
        loopLengthNeedsAttention = false;
        repaint();
    };
    loopLengthSlider.onDragEnd = [this]
    {
        loopLengthNeedsAttention = false;
        repaint();
    };

    controlsContent.addAndMakeVisible (calculatedBpmLabel);
    calculatedBpmLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (manualBpmOverrideToggle);
    manualBpmOverrideToggle.setToggleState (processor.getManualBpmOverrideEnabled(), juce::dontSendNotification);
    manualBpmOverrideToggle.onClick = [this]
    {
        processor.setManualBpmOverrideEnabled (manualBpmOverrideToggle.getToggleState());
        updateManualBpmOverrideVisibility();
        updateAfterSampleOrSliceChange(); // refreshes the "~X BPM" label immediately
    };

    controlsContent.addAndMakeVisible (manualBpmOverrideLabel);
    manualBpmOverrideLabel.setText ("BPM", juce::dontSendNotification);
    manualBpmOverrideLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (manualBpmOverrideSlider);
    manualBpmOverrideSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    manualBpmOverrideSlider.setRange (20.0, 300.0, 0.1);
    manualBpmOverrideSlider.setValue (processor.getManualBpmOverrideValue(), juce::dontSendNotification);
    manualBpmOverrideSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    manualBpmOverrideSlider.onValueChange = [this]
    {
        processor.setManualBpmOverrideValue (manualBpmOverrideSlider.getValue());
        updateAfterSampleOrSliceChange(); // refreshes the "~X BPM" label live while dragging
    };

    controlsContent.addAndMakeVisible (pitchModeLabel);
    pitchModeLabel.setText ("Pitch mode", juce::dontSendNotification);
    pitchModeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (pitchModeSelector);
    pitchModeSelector.addItem ("Repitch", 1);
    pitchModeSelector.addItem ("Time-Stretch", 2);
    pitchModeSelector.addItem ("NoSync", 3);
    {
        const auto currentMode = processor.getPitchMode();
        const int selectedId = currentMode == SlicerAudioProcessor::PitchMode::timeStretch ? 2
                              : currentMode == SlicerAudioProcessor::PitchMode::noSync ? 3
                                                                                        : 1;
        pitchModeSelector.setSelectedId (selectedId, juce::dontSendNotification);
    }
    pitchModeSelector.onChange = [this]
    {
        const int selectedId = pitchModeSelector.getSelectedId();
        const auto mode = selectedId == 2 ? SlicerAudioProcessor::PitchMode::timeStretch
                         : selectedId == 3 ? SlicerAudioProcessor::PitchMode::noSync
                                            : SlicerAudioProcessor::PitchMode::repitch;
        processor.setPitchMode (mode);
        updatePitchModeVisibility();
    };

    controlsContent.addAndMakeVisible (beatQuantizeToggleRepitch);
    beatQuantizeToggleRepitch.setToggleState (processor.getBeatQuantizeSliceLengthEnabledRepitch(), juce::dontSendNotification);
    beatQuantizeToggleRepitch.onClick = [this]
    {
        processor.setBeatQuantizeSliceLengthEnabledRepitch (beatQuantizeToggleRepitch.getToggleState());
    };

    controlsContent.addAndMakeVisible (transposeLabel);
    transposeLabel.setText ("Transpose (semitones)", juce::dontSendNotification);
    transposeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (transposeSlider);
    transposeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    transposeSlider.setRange (-24.0, 24.0, 1.0);
    transposeSlider.setValue (processor.getTransposeSemitones(), juce::dontSendNotification);
    transposeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    transposeSlider.onValueChange = [this]
    {
        processor.setTransposeSemitones ((float) transposeSlider.getValue());
    };

    controlsContent.addAndMakeVisible (grainSizeLabel);
    grainSizeLabel.setText ("Grain size (ms)", juce::dontSendNotification);
    grainSizeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (grainSizeSlider);
    grainSizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    grainSizeSlider.setRange (20.0, 150.0, 1.0);
    grainSizeSlider.setValue (processor.getGrainSizeMs(), juce::dontSendNotification);
    grainSizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    grainSizeSlider.onValueChange = [this]
    {
        processor.setGrainSizeMs ((float) grainSizeSlider.getValue());
    };

    controlsContent.addAndMakeVisible (windowShapeLabel);
    windowShapeLabel.setText ("Window shape", juce::dontSendNotification);
    windowShapeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (windowShapeSelector);
    windowShapeSelector.addItem ("Hann", 1);
    windowShapeSelector.addItem ("Triangular", 2);
    windowShapeSelector.setSelectedId (processor.getGrainWindowShape() == SlicerAudioProcessor::GrainWindowShape::triangular ? 2 : 1,
                                        juce::dontSendNotification);
    windowShapeSelector.onChange = [this]
    {
        const bool triangular = windowShapeSelector.getSelectedId() == 2;
        processor.setGrainWindowShape (triangular ? SlicerAudioProcessor::GrainWindowShape::triangular
                                                    : SlicerAudioProcessor::GrainWindowShape::hann);
    };

    controlsContent.addAndMakeVisible (beatQuantizeToggle);
    beatQuantizeToggle.setToggleState (processor.getBeatQuantizeSliceLengthEnabled(), juce::dontSendNotification);
    beatQuantizeToggle.onClick = [this]
    {
        processor.setBeatQuantizeSliceLengthEnabled (beatQuantizeToggle.getToggleState());
    };

    controlsContent.addAndMakeVisible (pitchShiftLabel);
    pitchShiftLabel.setText ("Pitch shift (semitones)", juce::dontSendNotification);
    pitchShiftLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (pitchShiftSlider);
    pitchShiftSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    pitchShiftSlider.setRange (-24.0, 24.0, 1.0);
    pitchShiftSlider.setValue (processor.getPitchShiftSemitones(), juce::dontSendNotification);
    pitchShiftSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    pitchShiftSlider.onValueChange = [this]
    {
        processor.setPitchShiftSemitones ((float) pitchShiftSlider.getValue());
    };

    controlsContent.addAndMakeVisible (sensitivityLabel);
    sensitivityLabel.setText ("Transient sensitivity", juce::dontSendNotification);
    sensitivityLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (sensitivitySlider);
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

    controlsContent.addAndMakeVisible (fadeInLabel);
    fadeInLabel.setText ("Fade in (ms)", juce::dontSendNotification);
    fadeInLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (fadeInSlider);
    fadeInSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeInSlider.setRange (0.0, 100.0, 0.5);
    fadeInSlider.setValue (processor.getFadeInMs(), juce::dontSendNotification);
    fadeInSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    fadeInSlider.onValueChange = [this]
    {
        processor.setFadeInMs ((float) fadeInSlider.getValue());
    };

    controlsContent.addAndMakeVisible (fadeOutLabel);
    fadeOutLabel.setText ("Fade out (ms)", juce::dontSendNotification);
    fadeOutLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (fadeOutSlider);
    fadeOutSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeOutSlider.setRange (0.0, 100.0, 0.5);
    fadeOutSlider.setValue (processor.getFadeOutMs(), juce::dontSendNotification);
    fadeOutSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    fadeOutSlider.onValueChange = [this]
    {
        processor.setFadeOutMs ((float) fadeOutSlider.getValue());
    };

    controlsContent.addAndMakeVisible (triggerModeLabel);
    triggerModeLabel.setText ("Trigger mode", juce::dontSendNotification);
    triggerModeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (triggerModeSelector);
    triggerModeSelector.addItem ("Slice Length", 1);
    triggerModeSelector.addItem ("Clock", 2);
    triggerModeSelector.setSelectedId (processor.getTriggerMode() == SlicerAudioProcessor::TriggerMode::clock ? 2 : 1,
                                        juce::dontSendNotification);
    triggerModeSelector.onChange = [this]
    {
        const bool clock = triggerModeSelector.getSelectedId() == 2;
        processor.setTriggerMode (clock ? SlicerAudioProcessor::TriggerMode::clock
                                         : SlicerAudioProcessor::TriggerMode::sliceLength);
        updateTriggerModeVisibility();
    };

    controlsContent.addAndMakeVisible (playbackStyleLabel);
    playbackStyleLabel.setText ("Playback style", juce::dontSendNotification);
    playbackStyleLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (playbackStyleGrid);

    controlsContent.addAndMakeVisible (clockReferenceLabel);
    clockReferenceLabel.setText ("Clock reference", juce::dontSendNotification);
    clockReferenceLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (clockReferenceSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        clockReferenceSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    clockReferenceSelector.setSelectedId (processor.getClockReferenceIndex() + 1, juce::dontSendNotification);
    clockReferenceSelector.onChange = [this]
    {
        processor.setClockReferenceIndex (clockReferenceSelector.getSelectedId() - 1);
    };

    controlsContent.addAndMakeVisible (tapeStopScopeLabel);
    tapeStopScopeLabel.setText ("Tape Stop scope", juce::dontSendNotification);
    tapeStopScopeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (tapeStopScopeSelector);
    for (int i = 0; i < SlicerAudioProcessor::numTapeStopScopeOptions; ++i)
        tapeStopScopeSelector.addItem (SlicerAudioProcessor::getTapeStopScopeName (i), i + 1); // JUCE item IDs are 1-based
    tapeStopScopeSelector.setSelectedId (processor.getTapeStopScope() == SlicerAudioProcessor::TapeStopScope::perTick ? 2 : 1,
                                          juce::dontSendNotification);
    tapeStopScopeSelector.onChange = [this]
    {
        processor.setTapeStopScope (tapeStopScopeSelector.getSelectedId() == 2
                                         ? SlicerAudioProcessor::TapeStopScope::perTick
                                         : SlicerAudioProcessor::TapeStopScope::wholeWindow);
    };

    controlsContent.addAndMakeVisible (filterSweepScopeLabel);
    filterSweepScopeLabel.setText ("Filter Sweep scope", juce::dontSendNotification);
    filterSweepScopeLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (filterSweepScopeSelector);
    for (int i = 0; i < SlicerAudioProcessor::numFilterSweepScopeOptions; ++i)
        filterSweepScopeSelector.addItem (SlicerAudioProcessor::getFilterSweepScopeName (i), i + 1); // JUCE item IDs are 1-based
    filterSweepScopeSelector.setSelectedId (processor.getFilterSweepScope() == SlicerAudioProcessor::FilterSweepScope::perTick ? 2 : 1,
                                             juce::dontSendNotification);
    filterSweepScopeSelector.onChange = [this]
    {
        processor.setFilterSweepScope (filterSweepScopeSelector.getSelectedId() == 2
                                            ? SlicerAudioProcessor::FilterSweepScope::perTick
                                            : SlicerAudioProcessor::FilterSweepScope::wholeWindow);
    };

    controlsContent.addAndMakeVisible (resetEveryLabel);
    resetEveryLabel.setText ("Reset every", juce::dontSendNotification);
    resetEveryLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (resetEverySelector);
    for (int i = 0; i < SlicerAudioProcessor::numResetBarsOptions; ++i)
        resetEverySelector.addItem (SlicerAudioProcessor::getResetBarsName (i), i + 1); // JUCE item IDs are 1-based
    resetEverySelector.setSelectedId (processor.getResetBarsIndex() + 1, juce::dontSendNotification);
    resetEverySelector.onChange = [this]
    {
        processor.setResetBarsIndex (resetEverySelector.getSelectedId() - 1);
    };

    controlsContent.addAndMakeVisible (subdivisionTableLabel);
    subdivisionTableLabel.setText ("Subdivision probability", juce::dontSendNotification);
    subdivisionTableLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (subdivisionGrid);

    updateTriggerModeVisibility();
    updatePitchModeVisibility();
    updateManualBpmOverrideVisibility();

    // Zoom/pan (Step 31) — live directly on the editor, not controlsContent,
    // so they stay visible next to the waveform regardless of scroll
    // position, same reasoning as waveformDisplay itself living there.
    addAndMakeVisible (zoomToTrimsButton);
    zoomToTrimsButton.addListener (this);

    addAndMakeVisible (resetZoomButton);
    resetZoomButton.addListener (this);

    addAndMakeVisible (waveformDisplay);
    waveformDisplay.onSampleChanged = [this] { updateAfterSampleOrSliceChange(); };
    waveformDisplay.onTrimChanged = [this]
    {
        updateAfterSampleOrSliceChange();

        // Loop Length (bars) doesn't auto-adjust when the trim range
        // changes -- there's no way to guess the right new value -- so
        // flag it as needing a fresh look instead (Step 33), cleared the
        // moment the user touches loopLengthSlider (see its onValueChange/
        // onDragEnd above).
        loopLengthNeedsAttention = true;
        repaint();
    };

    // Fixed window size regardless of how much lives inside controlsContent
    // (it scrolls internally within controlsViewport's fixed height) —
    // this no longer needs to grow every time a control gets added, and
    // comfortably fits any modern laptop screen, 16" MacBook included.
    // Widened from 600 (Step 31) to give the waveform display significantly
    // more horizontal room for zoom/pan and the beat-number grid.
    setSize (900, 780);

    if (processor.hasSample())
        updateAfterSampleOrSliceChange();
}

SlicerAudioProcessorEditor::~SlicerAudioProcessorEditor()
{
    loadButton.removeListener (this);
    resetEditsButton.removeListener (this);
    undoButton.removeListener (this);
    redoButton.removeListener (this);
    auditionButton.removeListener (this);
    zoomToTrimsButton.removeListener (this);
    resetZoomButton.removeListener (this);
}

void SlicerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (14.0f);
    g.drawFittedText ("NeditVST — step 34: Mandatory periodic reset for Slice Length mode",
                       getLocalBounds().removeFromTop (30), juce::Justification::centred, 1);

    // Loop Length staleness highlight (Step 33). loopLengthLabel/Slider
    // live inside controlsContent, scrolled by controlsViewport -- rather
    // than computing their position by hand, getLocalArea() walks the
    // whole parent chain (including the viewport's current scroll
    // offset) to get their real on-screen rectangle in THIS component's
    // coordinate space, so the highlight tracks correctly regardless of
    // scroll position. Clipped to the viewport's own bounds so nothing
    // is drawn if the control is currently scrolled out of view, and
    // skipped entirely while hidden (Pitch Mode == NoSync, which hides
    // both — see updatePitchModeVisibility()).
    if (loopLengthNeedsAttention && loopLengthLabel.isVisible())
    {
        juce::Graphics::ScopedSaveState saveState (g);
        g.reduceClipRegion (controlsViewport.getBounds());

        const auto labelBounds = getLocalArea (&loopLengthLabel, loopLengthLabel.getLocalBounds());
        const auto sliderBounds = getLocalArea (&loopLengthSlider, loopLengthSlider.getLocalBounds());
        const auto highlightBounds = labelBounds.getUnion (sliderBounds).expanded (4);

        g.setColour (juce::Colours::orange);
        g.drawRect (highlightBounds, 2);
    }
}

void SlicerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (30); // space for the paint() header text

    controlsViewport.setBounds (area.removeFromTop (controlsViewportHeight));
    area.removeFromTop (20);

    // Vertical scrolling only (setScrollBarsShown (true, false) in the
    // constructor) — content is exactly as wide as the visible area minus
    // whatever room the vertical scrollbar itself needs, so nothing ever
    // needs to scroll sideways too.
    const int contentWidth = controlsViewport.getWidth() - controlsViewport.getScrollBarThickness();
    const int contentHeight = layoutControlsContent (contentWidth);
    controlsContent.setSize (contentWidth, contentHeight);

    // Zoom/pan (Step 31) — a small fixed row above the waveform, always
    // visible alongside it regardless of controlsContent's scroll position.
    auto zoomButtonsRow = area.removeFromTop (30);
    zoomToTrimsButton.setBounds (zoomButtonsRow.removeFromLeft (150));
    zoomButtonsRow.removeFromLeft (10);
    resetZoomButton.setBounds (zoomButtonsRow.removeFromLeft (150));
    area.removeFromTop (10);

    waveformDisplay.setBounds (area); // takes up all remaining space, always fully visible
}

int SlicerAudioProcessorEditor::layoutControlsContent (int contentWidth)
{
    // Every control below lives inside controlsContent, not the editor
    // itself — this lays them out in ITS local coordinate space (starting
    // at 0,0) exactly the way they used to lay out directly in the
    // editor's own area, then reports how much total height they actually
    // needed so the caller can size controlsContent to fit (it scrolls
    // inside controlsViewport's fixed height rather than the window
    // growing to match).
    constexpr int sacrificialHeight = 4000; // comfortably larger than any realistic total content height
    juce::Rectangle<int> area (0, 0, contentWidth, sacrificialHeight);

    auto topButtonRow = area.removeFromTop (40);
    loadButton.setBounds (topButtonRow.removeFromLeft (topButtonRow.getWidth() - 110));
    topButtonRow.removeFromLeft (10);
    resetEditsButton.setBounds (topButtonRow);
    area.removeFromTop (10);
    auditionButton.setBounds (area.removeFromTop (30));
    area.removeFromTop (10);
    statusLabel.setBounds (area.removeFromTop (30));
    area.removeFromTop (20);

    auto loopRow = area.removeFromTop (30);
    loopLengthLabel.setBounds (loopRow.removeFromLeft (140));
    loopLengthSlider.setBounds (loopRow.removeFromLeft (100));
    loopRow.removeFromLeft (10);
    calculatedBpmLabel.setBounds (loopRow);
    area.removeFromTop (10);

    auto manualBpmToggleRow = area.removeFromTop (24);
    manualBpmOverrideToggle.setBounds (manualBpmToggleRow);
    area.removeFromTop (6);

    auto manualBpmValueRow = area.removeFromTop (30);
    manualBpmOverrideLabel.setBounds (manualBpmValueRow.removeFromLeft (140));
    manualBpmOverrideSlider.setBounds (manualBpmValueRow);
    area.removeFromTop (20);

    auto pitchModeRow = area.removeFromTop (30);
    pitchModeLabel.setBounds (pitchModeRow.removeFromLeft (140));
    pitchModeSelector.setBounds (pitchModeRow.removeFromLeft (150));
    area.removeFromTop (10);

    beatQuantizeToggleRepitch.setBounds (area.removeFromTop (24));
    area.removeFromTop (10);

    auto transposeRow = area.removeFromTop (30);
    transposeLabel.setBounds (transposeRow.removeFromLeft (140));
    transposeSlider.setBounds (transposeRow);
    area.removeFromTop (10);

    auto grainSizeRow = area.removeFromTop (30);
    grainSizeLabel.setBounds (grainSizeRow.removeFromLeft (140));
    grainSizeSlider.setBounds (grainSizeRow);
    area.removeFromTop (10);

    auto windowShapeRow = area.removeFromTop (30);
    windowShapeLabel.setBounds (windowShapeRow.removeFromLeft (140));
    windowShapeSelector.setBounds (windowShapeRow.removeFromLeft (150));
    area.removeFromTop (10);

    beatQuantizeToggle.setBounds (area.removeFromTop (24));
    area.removeFromTop (10);

    auto pitchShiftRow = area.removeFromTop (30);
    pitchShiftLabel.setBounds (pitchShiftRow.removeFromLeft (140));
    pitchShiftSlider.setBounds (pitchShiftRow);
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

    auto triggerModeRow = area.removeFromTop (30);
    triggerModeLabel.setBounds (triggerModeRow.removeFromLeft (140));
    triggerModeSelector.setBounds (triggerModeRow.removeFromLeft (150));
    area.removeFromTop (10);

    playbackStyleLabel.setBounds (area.removeFromTop (20));
    playbackStyleGrid.setBounds (area.removeFromTop (PlaybackStyleGrid::getPreferredHeight()));
    area.removeFromTop (20);

    auto clockReferenceRow = area.removeFromTop (30);
    clockReferenceLabel.setBounds (clockReferenceRow.removeFromLeft (140));
    clockReferenceSelector.setBounds (clockReferenceRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto tapeStopScopeRow = area.removeFromTop (30);
    tapeStopScopeLabel.setBounds (tapeStopScopeRow.removeFromLeft (140));
    tapeStopScopeSelector.setBounds (tapeStopScopeRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto filterSweepScopeRow = area.removeFromTop (30);
    filterSweepScopeLabel.setBounds (filterSweepScopeRow.removeFromLeft (140));
    filterSweepScopeSelector.setBounds (filterSweepScopeRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto resetEveryRow = area.removeFromTop (30);
    resetEveryLabel.setBounds (resetEveryRow.removeFromLeft (140));
    resetEverySelector.setBounds (resetEveryRow.removeFromLeft (150));
    area.removeFromTop (10);

    subdivisionTableLabel.setBounds (area.removeFromTop (20));
    subdivisionGrid.setBounds (area.removeFromTop (SubdivisionProbabilityGrid::getPreferredHeight()));
    area.removeFromTop (20);

    auto undoRedoRow = area.removeFromTop (30);
    undoButton.setBounds (undoRedoRow.removeFromLeft (100));
    undoRedoRow.removeFromLeft (10);
    redoButton.setBounds (undoRedoRow.removeFromLeft (100));
    area.removeFromTop (10);

    return sacrificialHeight - area.getHeight();
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
    else if (button == &auditionButton)
    {
        processor.setAuditionActive (! processor.getAuditionActive());
    }
    else if (button == &zoomToTrimsButton)
    {
        waveformDisplay.zoomToTrims();
    }
    else if (button == &resetZoomButton)
    {
        waveformDisplay.resetZoom();
    }
}

void SlicerAudioProcessorEditor::timerCallback()
{
    undoButton.setEnabled (processor.canUndoEdit());
    redoButton.setEnabled (processor.canRedoEdit());

    // Polled rather than driven only by the button's own click, since the
    // processor can also stop an audition on its own (host transport
    // started) — the label/colour has to reflect that auto-stop too.
    const bool auditioning = processor.getAuditionActive();
    auditionButton.setButtonText (auditioning ? "Stop Audition" : "Audition");
    auditionButton.setColour (juce::TextButton::buttonColourId,
                               auditioning ? juce::Colours::orange.withAlpha (0.6f)
                                           : getLookAndFeel().findColour (juce::TextButton::buttonColourId));
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

void SlicerAudioProcessorEditor::updateTriggerModeVisibility()
{
    const bool clock = triggerModeSelector.getSelectedId() == 2;

    clockReferenceLabel.setVisible (clock);
    clockReferenceSelector.setVisible (clock);
    tapeStopScopeLabel.setVisible (clock);
    tapeStopScopeSelector.setVisible (clock);
    filterSweepScopeLabel.setVisible (clock);
    filterSweepScopeSelector.setVisible (clock);
    subdivisionTableLabel.setVisible (clock);
    subdivisionGrid.setVisible (clock);

    // Reset every (Step 34) — the mirror image of the Clock-only controls
    // above: Slice Length mode only, since Clock mode already has its own
    // window-boundary mechanism and doesn't need this at all.
    resetEveryLabel.setVisible (! clock);
    resetEverySelector.setVisible (! clock);
}

void SlicerAudioProcessorEditor::updateManualBpmOverrideVisibility()
{
    const bool enabled = manualBpmOverrideToggle.getToggleState();

    manualBpmOverrideLabel.setVisible (enabled);
    manualBpmOverrideSlider.setVisible (enabled);
}

void SlicerAudioProcessorEditor::updatePitchModeVisibility()
{
    const int selectedId = pitchModeSelector.getSelectedId();
    const bool repitch = selectedId == 1;
    const bool timeStretch = selectedId == 2;
    const bool noSync = selectedId == 3;

    beatQuantizeToggleRepitch.setVisible (repitch);

    grainSizeLabel.setVisible (timeStretch);
    grainSizeSlider.setVisible (timeStretch);
    windowShapeLabel.setVisible (timeStretch);
    windowShapeSelector.setVisible (timeStretch);
    beatQuantizeToggle.setVisible (timeStretch);
    pitchShiftLabel.setVisible (timeStretch);
    pitchShiftSlider.setVisible (timeStretch);

    transposeLabel.setVisible (noSync);
    transposeSlider.setVisible (noSync);

    // NoSync has zero tempo math, so none of this applies there (Step 27)
    // — trim markers and Audition, by contrast, stay available regardless
    // of Pitch Mode (they live on/near the waveform, not in this section,
    // so nothing to hide here for those).
    loopLengthLabel.setVisible (! noSync);
    loopLengthSlider.setVisible (! noSync);
    calculatedBpmLabel.setVisible (! noSync);
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
