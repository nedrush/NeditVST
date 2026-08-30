#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PlaybackStylePalette.h"

namespace
{
    // Pass 7 window-sizing constants.
    constexpr int windowMargin = 20;         // matches getLocalBounds().reduced (windowMargin) below
    constexpr int headerTextHeight = 30;     // space for paint()'s header text
    constexpr int layoutGap = 14;            // vertical gap between the waveform panel/sub-mode tab row/Layer 5
    constexpr int minContentWidth = 1160;    // the window's fixed content width -- generous enough for the waveform toolbar's full control row and Sequence's pattern-bank row (1080 was too tight -- the toolbar row's content plus its right-aligned Pitch Mode/Zoom cluster slightly overflowed it, squeezing the Quantize checkbox down to a sliver)
}

SlicerAudioProcessorEditor::SlicerAudioProcessorEditor (SlicerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      waveformDisplay (p),
      effectsBusPanel (p),
      playbackStyleFaderRow (p),
      playbackStyleParameterPanel (p),
      subdivisionGrid (p),
      playbackStyleSwatchRow (p), sequencePlaybackStyleFaderRow (p),
      sequencerBankSource (p), patternBankPanel (sequencerBankSource),
      sequencerGrid (p),
      performanceStyleParameterPanel (p,
          [&p] (int i) { return p.getPerformanceWorkingParameterValue (i); },
          [&p] (int i, float v) { p.setPerformanceWorkingParameterValue (i, v); },
          p.getPerformanceWorkingStyle() + 1,
          [&p] (int style) { p.setPerformanceWorkingStyle (style); }),
      performanceKeyboardSource (p), performanceKeyboardPanel (performanceKeyboardSource),
      controlStyleParameterPanel (p),
      controlKeyboardSource (*this), controlKeyboardPanel (controlKeyboardSource)
{
    addAndMakeVisible (controlsContent);
    controlsContent.setLookAndFeel (&neditLookAndFeel);

    addAndMakeVisible (subModeViewport);
    subModeViewport.setViewedComponent (&subModeContent, false); // we own it, don't let the viewport delete it
    subModeViewport.setScrollBarsShown (true, false); // vertical only, safety net -- see this class's own doc comment
    subModeContent.setLookAndFeel (&neditLookAndFeel);

    controlsContent.addAndMakeVisible (subModeTabs);
    subModeTabs.setOptions ({ { "Generate", std::nullopt }, { "Sequence", std::nullopt },
                               { "Control", std::nullopt }, { "Perform", std::nullopt } });

    // Seed both tab rows from the processor's own current trigger mode so
    // the initial syncTriggerModeToActiveTab() call below is a no-op.
    {
        const auto mode = processor.getTriggerMode();
        const int subModeIndex = mode == SlicerAudioProcessor::TriggerMode::sequenced ? 1
                                : mode == SlicerAudioProcessor::TriggerMode::control ? 2
                                : mode == SlicerAudioProcessor::TriggerMode::performance ? 3
                                                                                          : 0; // Generate
        subModeTabs.setSelectedIndex (subModeIndex, juce::dontSendNotification);

        if (mode == SlicerAudioProcessor::TriggerMode::clock)
            lastGenerateTriggerMode = SlicerAudioProcessor::TriggerMode::clock;
    }

    subModeTabs.onSelectionChanged = [this] (int) { updateActiveTabVisibility(); syncTriggerModeToActiveTab(); updateWindowSize(); };

    //=== Waveform panel (compact) -- Load/Audition/Loop Length+BPM/Fade/
    // Quantize/Zoom/Pitch Mode, all folded into one slim toolbar row above
    // waveformDisplay, both wrapped in waveformPanelBackdrop. ===
    controlsContent.addAndMakeVisible (waveformPanelBackdrop);

    controlsContent.addAndMakeVisible (loadButton);
    loadButton.addListener (this);
    loadButton.setTooltip ("Load Sample...");

    controlsContent.addAndMakeVisible (auditionButton);
    auditionButton.addListener (this);

    startTimerHz (10); // keeps Undo/Redo enabled-state (and the Audition button's label/colour) in sync with the processor

    controlsContent.addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
    statusLabel.setText (processor.hasSample() ? processor.getLoadedFileName()
                                                : "No sample loaded",
                          juce::dontSendNotification);

    controlsContent.addAndMakeVisible (loopLengthLabel);
    loopLengthLabel.setText ("Bars", juce::dontSendNotification);
    loopLengthLabel.setJustificationType (juce::Justification::centredLeft);
    loopLengthLabel.setFont (juce::Font (juce::FontOptions (11.0f)));

    controlsContent.addAndMakeVisible (loopLengthField);
    loopLengthField.setRange (1.0, 8.0, 1.0);
    loopLengthField.setNumDecimalPlacesToDisplay (0);
    loopLengthField.setValue (processor.getLoopLengthBars(), juce::dontSendNotification);
    loopLengthField.onValueChange = [this]
    {
        processor.setLoopLengthBars ((int) loopLengthField.getValue());

        const double bpm = processor.getCalculatedOriginalBpm();
        calculatedBpmLabel.setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                     juce::dontSendNotification);

        // Any interaction with this control counts as acknowledgment
        // (Step 33) -- even re-entering the same value -- so the staleness
        // highlight clears the moment the user looks at it.
        loopLengthNeedsAttention = false;
        repaint();
    };
    loopLengthField.onDragEnd = [this]
    {
        loopLengthNeedsAttention = false;
        repaint();
    };

    controlsContent.addAndMakeVisible (calculatedBpmLabel);
    calculatedBpmLabel.setJustificationType (juce::Justification::centredLeft);
    calculatedBpmLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    calculatedBpmLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));

    controlsContent.addAndMakeVisible (fadeInLabel);
    fadeInLabel.setText ("Fade", juce::dontSendNotification);
    fadeInLabel.setJustificationType (juce::Justification::centredLeft);
    fadeInLabel.setFont (juce::Font (juce::FontOptions (11.0f)));

    controlsContent.addAndMakeVisible (fadeInSlider);
    fadeInSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeInSlider.setScrollWheelEnabled (false);
    fadeInSlider.setRange (0.0, 100.0, 0.5);
    fadeInSlider.setValue (processor.getFadeInMs(), juce::dontSendNotification);
    fadeInSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fadeInSlider.setTooltip ("Fade in (ms)");
    fadeInSlider.onValueChange = [this] { processor.setFadeInMs ((float) fadeInSlider.getValue()); };

    controlsContent.addAndMakeVisible (fadeOutSlider);
    fadeOutSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeOutSlider.setScrollWheelEnabled (false);
    fadeOutSlider.setRange (0.0, 100.0, 0.5);
    fadeOutSlider.setValue (processor.getFadeOutMs(), juce::dontSendNotification);
    fadeOutSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fadeOutSlider.setTooltip ("Fade out (ms)");
    fadeOutSlider.onValueChange = [this] { processor.setFadeOutMs ((float) fadeOutSlider.getValue()); };

    controlsContent.addAndMakeVisible (quantizeTransientsToggle);
    quantizeTransientsToggle.setToggleState (processor.getQuantizeTransientsEnabled(), juce::dontSendNotification);
    quantizeTransientsToggle.onClick = [this]
    {
        processor.setQuantizeTransientsEnabled (quantizeTransientsToggle.getToggleState());
        updateQuantizeTransientsVisibility();
        updateAfterSampleOrSliceChange();
    };

    controlsContent.addAndMakeVisible (advancedSettingsButton);
    advancedSettingsButton.onClick = [this] { showWaveformAdvancedPopover(); };

    controlsContent.addAndMakeVisible (zoomToTrimsButton);
    zoomToTrimsButton.addListener (this);
    zoomToTrimsButton.setTooltip ("Zoom to Trims");

    controlsContent.addAndMakeVisible (resetZoomButton);
    resetZoomButton.addListener (this);
    resetZoomButton.setTooltip ("Reset Zoom");

    controlsContent.addAndMakeVisible (pitchModeSegments);
    pitchModeSegments.setOptions ({ { "Repitch", std::nullopt }, { "Time-Stretch", std::nullopt } });
    {
        const auto currentMode = processor.getPitchMode();
        const int selectedIndex = currentMode == SlicerAudioProcessor::PitchMode::timeStretch ? 1 : 0;
        pitchModeSegments.setSelectedIndex (selectedIndex, juce::dontSendNotification);
    }
    pitchModeSegments.onSelectionChanged = [this] (int selectedIndex)
    {
        const auto mode = selectedIndex == 1 ? SlicerAudioProcessor::PitchMode::timeStretch
                                              : SlicerAudioProcessor::PitchMode::repitch;
        processor.setPitchMode (mode);
        updatePitchModeVisibility();
    };

    controlsContent.addAndMakeVisible (waveformDisplay);
    waveformDisplay.onSampleChanged = [this] { updateAfterSampleOrSliceChange(); };
    waveformDisplay.onTrimChanged = [this]
    {
        updateAfterSampleOrSliceChange();
        loopLengthNeedsAttention = true;
        repaint();
    };

    //=== Delay/Reverb send bus panel (Pass 1) -- persistent, lives directly
    // in controlsContent so it stays visible regardless of which sub-mode
    // tab is active, same as waveformDisplay just above. ===
    controlsContent.addAndMakeVisible (effectsBusBackdrop);
    controlsContent.addAndMakeVisible (effectsBusPanel);

    //=== Advanced popover controls (secondary waveform-panel settings) ===
    // Constructed/wired here exactly as before, but deliberately NOT parented
    // to anything yet -- see showWaveformAdvancedPopover()'s own doc comment
    // for why they're reparented on demand instead.
    resetEditsButton.addListener (this);

    undoButton.addListener (this);
    undoButton.setEnabled (false);

    redoButton.addListener (this);
    redoButton.setEnabled (false);

    manualBpmOverrideToggle.setToggleState (processor.getManualBpmOverrideEnabled(), juce::dontSendNotification);
    manualBpmOverrideToggle.onClick = [this]
    {
        processor.setManualBpmOverrideEnabled (manualBpmOverrideToggle.getToggleState());
        updateManualBpmOverrideVisibility();
        updateAfterSampleOrSliceChange();
    };

    manualBpmOverrideLabel.setText ("BPM", juce::dontSendNotification);
    manualBpmOverrideLabel.setJustificationType (juce::Justification::centredLeft);

    manualBpmOverrideSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    manualBpmOverrideSlider.setScrollWheelEnabled (false);
    manualBpmOverrideSlider.setRange (20.0, 300.0, 0.1);
    manualBpmOverrideSlider.setValue (processor.getManualBpmOverrideValue(), juce::dontSendNotification);
    manualBpmOverrideSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    manualBpmOverrideSlider.onValueChange = [this]
    {
        processor.setManualBpmOverrideValue (manualBpmOverrideSlider.getValue());
        updateAfterSampleOrSliceChange();
    };

    sensitivityLabel.setText ("Transient sensitivity", juce::dontSendNotification);
    sensitivityLabel.setJustificationType (juce::Justification::centredLeft);

    sensitivityField.setRange (0.0, 1.0, 0.01);
    sensitivityField.setNumDecimalPlacesToDisplay (2);
    sensitivityField.setValue (processor.getSensitivity(), juce::dontSendNotification);
    sensitivityField.onValueChange = [this]
    {
        // See the original Pass-4 doc comment (now here): committing on
        // every value change fired far too often while dragging, so this
        // only shows a live PREVIEW while dragging and commits for real on
        // release or for non-drag changes.
        if (sensitivityField.isMouseButtonDown())
        {
            auto preview = processor.previewSlicesAtSensitivity ((float) sensitivityField.getValue());
            waveformDisplay.showPreviewSlices (preview);
            return;
        }

        processor.setSensitivityAndRedetect ((float) sensitivityField.getValue());
        updateAfterSampleOrSliceChange();
    };
    sensitivityField.onDragEnd = [this]
    {
        processor.setSensitivityAndRedetect ((float) sensitivityField.getValue());
        updateAfterSampleOrSliceChange();
    };

    quantizeGridLabel.setText ("Grid", juce::dontSendNotification);
    quantizeGridLabel.setJustificationType (juce::Justification::centredLeft);

    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        quantizeGridSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    quantizeGridSelector.setSelectedId (processor.getQuantizeGridIndex() + 1, juce::dontSendNotification);
    quantizeGridSelector.onChange = [this]
    {
        processor.setQuantizeGridIndex (quantizeGridSelector.getSelectedId() - 1);
        updateAfterSampleOrSliceChange();
    };

    beatQuantizeToggleRepitch.setToggleState (processor.getBeatQuantizeSliceLengthEnabledRepitch(), juce::dontSendNotification);
    beatQuantizeToggleRepitch.onClick = [this]
    {
        processor.setBeatQuantizeSliceLengthEnabledRepitch (beatQuantizeToggleRepitch.getToggleState());
    };

    pitchModeExtraViewport.setViewedComponent (&pitchModeExtraContent, false); // we own it, don't let the viewport delete it
    pitchModeExtraViewport.setScrollBarsShown (true, false);
    pitchModeExtraContent.setLookAndFeel (&neditLookAndFeel);

    pitchModeExtraContent.addAndMakeVisible (grainSizeLabel);
    grainSizeLabel.setText ("Grain size (ms)", juce::dontSendNotification);
    grainSizeLabel.setJustificationType (juce::Justification::centredLeft);

    pitchModeExtraContent.addAndMakeVisible (grainSizeSlider);
    grainSizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    grainSizeSlider.setScrollWheelEnabled (false);
    grainSizeSlider.setRange (20.0, 150.0, 1.0);
    grainSizeSlider.setValue (processor.getGrainSizeMs(), juce::dontSendNotification);
    grainSizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    grainSizeSlider.onValueChange = [this] { processor.setGrainSizeMs ((float) grainSizeSlider.getValue()); };

    pitchModeExtraContent.addAndMakeVisible (windowShapeLabel);
    windowShapeLabel.setText ("Window shape", juce::dontSendNotification);
    windowShapeLabel.setJustificationType (juce::Justification::centredLeft);

    pitchModeExtraContent.addAndMakeVisible (windowShapeSelector);
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

    pitchModeExtraContent.addAndMakeVisible (beatQuantizeToggle);
    beatQuantizeToggle.setToggleState (processor.getBeatQuantizeSliceLengthEnabled(), juce::dontSendNotification);
    beatQuantizeToggle.onClick = [this]
    {
        processor.setBeatQuantizeSliceLengthEnabled (beatQuantizeToggle.getToggleState());
    };

    pitchModeExtraContent.addAndMakeVisible (pitchShiftLabel);
    pitchShiftLabel.setText ("Pitch shift (semitones)", juce::dontSendNotification);
    pitchShiftLabel.setJustificationType (juce::Justification::centredLeft);

    pitchModeExtraContent.addAndMakeVisible (pitchShiftSlider);
    pitchShiftSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    pitchShiftSlider.setScrollWheelEnabled (false);
    pitchShiftSlider.setRange (-24.0, 24.0, 1.0);
    pitchShiftSlider.setValue (processor.getPitchShiftSemitones(), juce::dontSendNotification);
    pitchShiftSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    pitchShiftSlider.onValueChange = [this] { processor.setPitchShiftSemitones ((float) pitchShiftSlider.getValue()); };

    //=== Generate tab -- style selector, fader row, revealed parameter
    // panel, then Generate's own local Timing section (unchanged). ===
    subModeContent.addAndMakeVisible (playbackStyleSegments);
    {
        std::vector<SegmentedButtonRow::Option> styleOptions;
        for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
            styleOptions.push_back ({ SlicerAudioProcessor::getPlaybackStyleName (i), PlaybackStylePalette::getStyleColour (i) });
        playbackStyleSegments.setOptions (std::move (styleOptions));
    }
    playbackStyleSegments.onSelectionChanged = [this] (int selectedIndex) { onGenerateStyleSelected (selectedIndex); };

    subModeContent.addAndMakeVisible (playbackStyleFaderRow);
    playbackStyleFaderRow.setSelectedStyle (playbackStyleSegments.getSelectedIndex());
    playbackStyleFaderRow.onStyleSelected = [this] (int styleIndex) { playbackStyleSegments.setSelectedIndex (styleIndex); };

    subModeContent.addAndMakeVisible (playbackStyleParametersLabel);
    playbackStyleParametersLabel.setText ("Style parameters", juce::dontSendNotification);
    playbackStyleParametersLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (generateParameterAreaBackdrop);

    subModeContent.addAndMakeVisible (playbackStyleParameterPanel);
    // playbackStyleSegments above is the real selector -- hide the panel's
    // own internal style ComboBox so the two don't sit redundantly on top
    // of each other (see setStyleSelectorVisible()'s own doc comment).
    playbackStyleParameterPanel.setStyleSelectorVisible (false);

    subModeContent.addAndMakeVisible (timingSectionPanel);

    subModeContent.addAndMakeVisible (sliceLengthClockLabel);
    sliceLengthClockLabel.setText ("Timing", juce::dontSendNotification);
    sliceLengthClockLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (sliceLengthClockSegments);
    sliceLengthClockSegments.setOptions ({ { "Slice Length", std::nullopt }, { "Clock", std::nullopt } });
    {
        const auto currentMode = processor.getTriggerMode();
        const int selectedIndex = currentMode == SlicerAudioProcessor::TriggerMode::clock ? 1 : 0;
        sliceLengthClockSegments.setSelectedIndex (selectedIndex, juce::dontSendNotification);
    }
    sliceLengthClockSegments.onSelectionChanged = [this] (int selectedIndex)
    {
        lastGenerateTriggerMode = selectedIndex == 1 ? SlicerAudioProcessor::TriggerMode::clock
                                                      : SlicerAudioProcessor::TriggerMode::sliceLength;
        syncTriggerModeToActiveTab();

        // No updateWindowSize() call here -- the Timing section's reserved
        // height is fixed to Clock's full content now regardless of which
        // sub-mode is selected (see layoutGenerateTab()'s own Timing
        // layout), so toggling Slice Length/Clock never needs to resize
        // the window; syncTriggerModeToActiveTab() above already updates
        // which rows are visible.
    };

    subModeContent.addAndMakeVisible (clockReferenceLabel);
    clockReferenceLabel.setText ("Clock reference", juce::dontSendNotification);
    clockReferenceLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (clockReferenceSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        clockReferenceSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1);
    clockReferenceSelector.setSelectedId (processor.getClockReferenceIndex() + 1, juce::dontSendNotification);
    clockReferenceSelector.onChange = [this]
    {
        processor.setClockReferenceIndex (clockReferenceSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (tapeStopScopeLabel);
    tapeStopScopeLabel.setText ("Tape Stop scope", juce::dontSendNotification);
    tapeStopScopeLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (tapeStopScopeSelector);
    for (int i = 0; i < SlicerAudioProcessor::numTapeStopScopeOptions; ++i)
        tapeStopScopeSelector.addItem (SlicerAudioProcessor::getTapeStopScopeName (i), i + 1);
    tapeStopScopeSelector.setSelectedId (processor.getTapeStopScope() == SlicerAudioProcessor::TapeStopScope::perTick ? 2 : 1,
                                          juce::dontSendNotification);
    tapeStopScopeSelector.onChange = [this]
    {
        processor.setTapeStopScope (tapeStopScopeSelector.getSelectedId() == 2
                                         ? SlicerAudioProcessor::TapeStopScope::perTick
                                         : SlicerAudioProcessor::TapeStopScope::wholeWindow);
    };

    subModeContent.addAndMakeVisible (filterSweepScopeLabel);
    filterSweepScopeLabel.setText ("Filter Sweep scope", juce::dontSendNotification);
    filterSweepScopeLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (filterSweepScopeSelector);
    for (int i = 0; i < SlicerAudioProcessor::numFilterSweepScopeOptions; ++i)
        filterSweepScopeSelector.addItem (SlicerAudioProcessor::getFilterSweepScopeName (i), i + 1);
    filterSweepScopeSelector.setSelectedId (processor.getFilterSweepScope() == SlicerAudioProcessor::FilterSweepScope::perTick ? 2 : 1,
                                             juce::dontSendNotification);
    filterSweepScopeSelector.onChange = [this]
    {
        processor.setFilterSweepScope (filterSweepScopeSelector.getSelectedId() == 2
                                            ? SlicerAudioProcessor::FilterSweepScope::perTick
                                            : SlicerAudioProcessor::FilterSweepScope::wholeWindow);
    };

    subModeContent.addAndMakeVisible (resetEveryLabel);
    resetEveryLabel.setText ("Reset every", juce::dontSendNotification);
    resetEveryLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (resetEverySelector);
    for (int i = 0; i < SlicerAudioProcessor::numResetBarsOptions; ++i)
        resetEverySelector.addItem (SlicerAudioProcessor::getResetBarsName (i), i + 1);
    resetEverySelector.setSelectedId (processor.getResetBarsIndex() + 1, juce::dontSendNotification);
    resetEverySelector.onChange = [this]
    {
        processor.setResetBarsIndex (resetEverySelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (subdivisionTableLabel);
    subdivisionTableLabel.setText ("Subdivision probability", juce::dontSendNotification);
    subdivisionTableLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (subdivisionGrid);

    //=== Sequence tab -- slim swatches/Randomize/Clear/pattern-info row,
    // then sequencerGrid at generous height, then the MIDI pattern bank. ===
    subModeContent.addAndMakeVisible (stepResolutionSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        stepResolutionSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1);
    stepResolutionSelector.setSelectedId (processor.getStepResolutionIndex() + 1, juce::dontSendNotification);
    stepResolutionSelector.onChange = [this]
    {
        processor.setStepResolutionIndex (stepResolutionSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (patternLengthSelector);
    for (int i = 0; i < SlicerAudioProcessor::numPatternLengthBarsOptions; ++i)
        patternLengthSelector.addItem (SlicerAudioProcessor::getPatternLengthBarsName (i), i + 1);
    patternLengthSelector.setSelectedId (processor.getPatternLengthBarsIndex() + 1, juce::dontSendNotification);
    patternLengthSelector.onChange = [this]
    {
        processor.setPatternLengthBarsIndex (patternLengthSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (randomizeSequenceButton);
    randomizeSequenceButton.addListener (this);

    subModeContent.addAndMakeVisible (clearSequenceButton);
    clearSequenceButton.addListener (this);

    subModeContent.addAndMakeVisible (playbackStyleSwatchRow);
    subModeContent.addAndMakeVisible (sequencePlaybackStyleFaderRow);
    subModeContent.addAndMakeVisible (patternBankPanel);

    subModeContent.addAndMakeVisible (patternSwitchTimingLabel);
    patternSwitchTimingLabel.setText ("Pattern switch timing", juce::dontSendNotification);
    patternSwitchTimingLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (patternSwitchTimingSelector);
    patternSwitchTimingSelector.addItem ("Immediate", 1);
    patternSwitchTimingSelector.addItem ("Set Interval", 2);
    patternSwitchTimingSelector.addItem ("End of Pattern", 3);
    patternSwitchTimingSelector.setSelectedId (
        static_cast<int> (processor.getPatternSwitchTiming()) + 1, juce::dontSendNotification);
    patternSwitchTimingSelector.onChange = [this]
    {
        processor.setPatternSwitchTiming (
            static_cast<SlicerAudioProcessor::PatternSwitchTiming> (patternSwitchTimingSelector.getSelectedId() - 1));
        updateActiveTabVisibility(); // Set Interval's own note-value picker only shows for that one timing mode
    };

    subModeContent.addAndMakeVisible (patternSwitchIntervalLabel);
    patternSwitchIntervalLabel.setText ("Switch interval", juce::dontSendNotification);
    patternSwitchIntervalLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (patternSwitchIntervalSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        patternSwitchIntervalSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1);
    patternSwitchIntervalSelector.setSelectedId (processor.getPatternSwitchIntervalIndex() + 1, juce::dontSendNotification);
    patternSwitchIntervalSelector.onChange = [this]
    {
        processor.setPatternSwitchIntervalIndex (patternSwitchIntervalSelector.getSelectedId() - 1);
    };

    //=== Perform tab (unchanged) ===
    subModeContent.addAndMakeVisible (performanceStyleParametersLabel);
    performanceStyleParametersLabel.setText ("Style parameters", juce::dontSendNotification);
    performanceStyleParametersLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (performanceStyleParameterPanel);

    subModeContent.addAndMakeVisible (performanceLoopToggle);
    performanceLoopToggle.setToggleState (processor.getPerformanceWorkingLoop(), juce::dontSendNotification);
    performanceLoopToggle.onClick = [this]
    {
        processor.setPerformanceWorkingLoop (performanceLoopToggle.getToggleState());
    };

    subModeContent.addAndMakeVisible (performanceSyncToggle);
    performanceSyncToggle.setToggleState (processor.getPerformanceWorkingSync(), juce::dontSendNotification);
    performanceSyncToggle.onClick = [this]
    {
        processor.setPerformanceWorkingSync (performanceSyncToggle.getToggleState());
    };

    subModeContent.addAndMakeVisible (performanceTrimSnapLabel);
    performanceTrimSnapLabel.setText ("Trim snap", juce::dontSendNotification);
    performanceTrimSnapLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (performanceTrimSnapSelector);
    performanceTrimSnapSelector.addItem ("Transients", 1);
    performanceTrimSnapSelector.addItem ("Grid", 2);
    performanceTrimSnapSelector.setSelectedId (
        static_cast<int> (processor.getPerformanceTrimSnapMode()) + 1, juce::dontSendNotification);
    performanceTrimSnapSelector.onChange = [this]
    {
        processor.setPerformanceTrimSnapMode (
            static_cast<SlicerAudioProcessor::TrimSnapMode> (performanceTrimSnapSelector.getSelectedId() - 1));
        updatePerformanceTrimSnapVisibility();
    };

    subModeContent.addAndMakeVisible (performanceTrimGridLabel);
    performanceTrimGridLabel.setText ("Grid", juce::dontSendNotification);
    performanceTrimGridLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (performanceTrimGridSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        performanceTrimGridSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1);
    performanceTrimGridSelector.setSelectedId (processor.getPerformanceTrimGridIndex() + 1, juce::dontSendNotification);
    performanceTrimGridSelector.onChange = [this]
    {
        processor.setPerformanceTrimGridIndex (performanceTrimGridSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (performanceQuantizeRecallToggle);
    performanceQuantizeRecallToggle.setToggleState (processor.getPerformanceQuantizeRecallEnabled(), juce::dontSendNotification);
    performanceQuantizeRecallToggle.onClick = [this]
    {
        processor.setPerformanceQuantizeRecallEnabled (performanceQuantizeRecallToggle.getToggleState());
        updatePerformanceQuantizeRecallVisibility();
    };

    subModeContent.addAndMakeVisible (performanceQuantizeRecallIntervalLabel);
    performanceQuantizeRecallIntervalLabel.setText ("Recall interval", juce::dontSendNotification);
    performanceQuantizeRecallIntervalLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (performanceQuantizeRecallIntervalSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        performanceQuantizeRecallIntervalSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1);
    performanceQuantizeRecallIntervalSelector.setSelectedId (processor.getPerformanceQuantizeRecallIntervalIndex() + 1, juce::dontSendNotification);
    performanceQuantizeRecallIntervalSelector.onChange = [this]
    {
        processor.setPerformanceQuantizeRecallIntervalIndex (performanceQuantizeRecallIntervalSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (performanceKeyboardPanel);

    //=== Control tab -- base note/gate header, wash legend, real piano
    // keyboard, revealed style parameters. ===
    subModeContent.addAndMakeVisible (controlBaseNoteLabel);
    controlBaseNoteLabel.setText ("Base note", juce::dontSendNotification);
    controlBaseNoteLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (controlBaseNoteSlider);
    controlBaseNoteSlider.setSliderStyle (juce::Slider::IncDecButtons);
    controlBaseNoteSlider.setRange (0.0, 127.0, 1.0);
    controlBaseNoteSlider.setScrollWheelEnabled (false);
    controlBaseNoteSlider.setValue (processor.getControlBaseNote(), juce::dontSendNotification);
    controlBaseNoteSlider.onValueChange = [this]
    {
        processor.setControlBaseNote ((int) controlBaseNoteSlider.getValue());
        updateControlBaseNoteDisplay(); // keyswitch notes are computed straight from the base note -- moving it shifts the whole block
    };

    subModeContent.addAndMakeVisible (controlBaseNoteNameLabel);
    controlBaseNoteNameLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (controlSliceRangeLabel);
    controlSliceRangeLabel.setJustificationType (juce::Justification::centredLeft);
    controlSliceRangeLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    controlSliceRangeLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));

    subModeContent.addAndMakeVisible (controlGateModeLabel);
    controlGateModeLabel.setText ("Trigger/Gate", juce::dontSendNotification);
    controlGateModeLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (controlGateModeSegments);
    controlGateModeSegments.setOptions ({ { "Trigger", std::nullopt }, { "Gate", std::nullopt } });
    controlGateModeSegments.setSelectedIndex (processor.getControlGateMode() ? 1 : 0, juce::dontSendNotification);
    controlGateModeSegments.onSelectionChanged = [this] (int selectedIndex)
    {
        processor.setControlGateMode (selectedIndex == 1);
    };

    subModeContent.addAndMakeVisible (controlLegendKeyswitchLabel);
    controlLegendKeyswitchLabel.setText ("Keyswitches (styles)", juce::dontSendNotification);
    controlLegendKeyswitchLabel.setJustificationType (juce::Justification::centredLeft);
    controlLegendKeyswitchLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    controlLegendKeyswitchLabel.setColour (juce::Label::textColourId, ControlKeyboardPanel::keyswitchWashColour.brighter (0.4f));

    subModeContent.addAndMakeVisible (controlLegendSliceLabel);
    controlLegendSliceLabel.setText ("Slices", juce::dontSendNotification);
    controlLegendSliceLabel.setJustificationType (juce::Justification::centredLeft);
    controlLegendSliceLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    controlLegendSliceLabel.setColour (juce::Label::textColourId, ControlKeyboardPanel::sliceWashColour.brighter (0.4f));

    subModeContent.addAndMakeVisible (controlKeyboardPanel);

    subModeContent.addAndMakeVisible (controlStyleParametersLabel);
    controlStyleParametersLabel.setText ("Style parameters", juce::dontSendNotification);
    controlStyleParametersLabel.setJustificationType (juce::Justification::centredLeft);

    // Bound to the same global default storage as Generate's own
    // playbackStyleParameterPanel (no getValue/setValue lambdas passed).
    // ControlKeyboardPanel's keyswitch keys are this panel's external style
    // selector, same relationship playbackStyleSegments has to Generate's
    // panel, so its own internal ComboBox stays hidden.
    subModeContent.addAndMakeVisible (controlStyleParameterPanel);
    controlStyleParameterPanel.setStyleSelectorVisible (false);

    updateControlBaseNoteDisplay();
    updateControlStyleParameterPanelVisibility();

    subModeContent.addAndMakeVisible (sequencerViewport);
    sequencerViewport.setViewedComponent (&sequencerGrid, false); // we own it, don't let the viewport delete it
    sequencerViewport.setScrollBarsShown (true, true);

    //=== Visibility groups -- populated now that every control referenced
    // is fully constructed, used purely for blanket setVisible() in
    // updateActiveTabVisibility(). ===
    generateComponents = {
        &playbackStyleSegments, &playbackStyleFaderRow, &playbackStyleParametersLabel,
        &generateParameterAreaBackdrop, &playbackStyleParameterPanel,
        &timingSectionPanel, &sliceLengthClockLabel, &sliceLengthClockSegments,
        &clockReferenceLabel, &clockReferenceSelector, &tapeStopScopeLabel, &tapeStopScopeSelector,
        &filterSweepScopeLabel, &filterSweepScopeSelector, &resetEveryLabel, &resetEverySelector,
        &subdivisionTableLabel, &subdivisionGrid
    };

    sequenceComponents = {
        &stepResolutionSelector, &patternLengthSelector,
        &randomizeSequenceButton, &clearSequenceButton, &playbackStyleSwatchRow, &sequencePlaybackStyleFaderRow,
        &sequencerViewport, &patternBankPanel,
        &patternSwitchTimingLabel, &patternSwitchTimingSelector, &patternSwitchIntervalLabel, &patternSwitchIntervalSelector
    };

    performComponents = {
        &performanceStyleParametersLabel, &performanceStyleParameterPanel, &performanceLoopToggle, &performanceSyncToggle,
        &performanceTrimSnapLabel, &performanceTrimSnapSelector, &performanceTrimGridLabel, &performanceTrimGridSelector,
        &performanceQuantizeRecallToggle, &performanceQuantizeRecallIntervalLabel, &performanceQuantizeRecallIntervalSelector,
        &performanceKeyboardPanel
    };

    controlComponents = {
        &controlBaseNoteLabel, &controlBaseNoteSlider, &controlBaseNoteNameLabel, &controlSliceRangeLabel,
        &controlGateModeLabel, &controlGateModeSegments,
        &controlLegendKeyswitchLabel, &controlLegendSliceLabel, &controlKeyboardPanel
    };

    syncTriggerModeToActiveTab(); // no-op: both tab rows were already seeded from processor.getTriggerMode() above
    updateActiveTabVisibility();

    // Initial sizing pass, now that every control exists and every tab row
    // is seeded/visible.
    updateWindowSize();

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
    randomizeSequenceButton.removeListener (this);
    clearSequenceButton.removeListener (this);
}

void SlicerAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.setFont (14.0f);
    g.drawFittedText ("NeditVST", getLocalBounds().removeFromTop (30), juce::Justification::centred, 1);

    // Loop Length staleness highlight (Step 33). getLocalArea() walks the
    // whole parent chain to get loopLengthLabel/Field's real on-screen
    // rectangle in THIS component's coordinate space, so the highlight
    // tracks correctly regardless of nesting.
    if (loopLengthNeedsAttention && loopLengthLabel.isVisible())
    {
        const auto labelBounds = getLocalArea (&loopLengthLabel, loopLengthLabel.getLocalBounds());
        const auto fieldBounds = getLocalArea (&loopLengthField, loopLengthField.getLocalBounds());
        const auto highlightBounds = labelBounds.getUnion (fieldBounds).expanded (4);

        g.setColour (juce::Colours::orange);
        g.drawRect (highlightBounds, 2);
    }
}

void SlicerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (windowMargin);
    area.removeFromTop (headerTextHeight); // space for paint()'s header text

    const int contentWidth = area.getWidth();
    const int contentHeight = layoutControlsContent (contentWidth);
    controlsContent.setBounds (area.removeFromTop (contentHeight));
    area.removeFromTop (layoutGap);

    // Layer 5 -- the one safety-net scrolling region (see this class's own
    // doc comment). Fixed VISIBLE height, reserved to the tallest tab's own
    // real preferred height (subModeViewportHeight, recomputed in
    // updateWindowSize()).
    subModeViewport.setBounds (area.removeFromTop (subModeViewportHeight));

    const int subModeContentWidth = subModeViewport.getWidth() - subModeViewport.getScrollBarThickness();
    const int subModeHeight = layoutSubModeContent (subModeContentWidth);
    subModeContent.setSize (subModeContentWidth, subModeHeight);
}

juce::Rectangle<int> SlicerAudioProcessorEditor::sectionContentArea (const SectionPanel& panel)
{
    return panel.getBounds().withTrimmedTop (SectionPanel::titleBarHeight).reduced (8, 6);
}

int SlicerAudioProcessorEditor::layoutControlsContent (int contentWidth)
{
    int y = layoutWaveformPanel (0, contentWidth);
    y += layoutGap;

    y = layoutEffectsBusPanel (y, contentWidth);
    y += layoutGap;

    subModeTabs.setBounds (0, y, contentWidth, SegmentedButtonRow::preferredHeight);
    y += SegmentedButtonRow::preferredHeight;

    return y;
}

int SlicerAudioProcessorEditor::layoutEffectsBusPanel (int startY, int width)
{
    const int panelHeight = EffectsBusPanel::getPreferredHeight();
    effectsBusBackdrop.setBounds (0, startY, width, panelHeight);
    effectsBusPanel.setBounds (0, startY, width, panelHeight);

    // Must return the new cumulative Y cursor (startY + panelHeight), NOT
    // just panelHeight -- layoutControlsContent() does `y = layoutEffectsBusPanel (y, ...)`,
    // reassigning its running Y cursor to this return value, so returning
    // only the consumed height would discard everything laid out before
    // this panel (the waveform panel above it). layoutWaveformPanel() gets
    // away with returning just its own height because it's only ever
    // called with startY == 0, the first thing in layoutControlsContent()
    // -- that's not a convention to copy for any panel after the first.
    return startY + panelHeight;
}

int SlicerAudioProcessorEditor::layoutWaveformPanel (int startY, int width)
{
    constexpr int pad = 10;
    constexpr int rowHeight = 26;
    constexpr int rowToWaveformGap = 8;
    constexpr int waveformHeight = 90;
    constexpr int controlGap = 8;

    const int panelHeight = pad * 2 + rowHeight + rowToWaveformGap + waveformHeight;
    waveformPanelBackdrop.setBounds (0, startY, width, panelHeight);

    juce::Rectangle<int> row (pad, startY + pad, width - pad * 2, rowHeight);

    // Right-aligned cluster (Pitch Mode + Zoom) carved out FIRST from the
    // same row, so whatever's laid out left-to-right afterward naturally
    // stops short of it -- no separate rectangle to keep in sync.
    constexpr int pitchModeWidth = 170;
    const int zoomToTrimsWidth = zoomToTrimsButton.getBestWidthForHeight (rowHeight) + 14;
    const int resetZoomWidth = resetZoomButton.getBestWidthForHeight (rowHeight) + 14;
    auto rightCluster = row.removeFromRight (pitchModeWidth + controlGap + zoomToTrimsWidth + resetZoomWidth + controlGap);
    pitchModeSegments.setBounds (rightCluster.removeFromLeft (pitchModeWidth));
    rightCluster.removeFromLeft (controlGap);
    zoomToTrimsButton.setBounds (rightCluster.removeFromLeft (zoomToTrimsWidth));
    resetZoomButton.setBounds (rightCluster.removeFromLeft (resetZoomWidth));

    auto place = [&row, controlGap] (juce::Component& c, int w)
    {
        c.setBounds (row.removeFromLeft (w));
        row.removeFromLeft (controlGap);
    };

    place (loadButton, loadButton.getBestWidthForHeight (rowHeight) + 18);
    place (auditionButton, auditionButton.getBestWidthForHeight (rowHeight) + 18);

    statusLabel.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (controlGap);

    loopLengthLabel.setBounds (row.removeFromLeft (32));
    loopLengthField.setBounds (row.removeFromLeft (30));
    row.removeFromLeft (4);
    calculatedBpmLabel.setBounds (row.removeFromLeft (72));
    row.removeFromLeft (controlGap);

    fadeInLabel.setBounds (row.removeFromLeft (32));
    fadeInSlider.setBounds (row.removeFromLeft (54));
    row.removeFromLeft (2);
    fadeOutSlider.setBounds (row.removeFromLeft (54));
    row.removeFromLeft (controlGap);

    advancedSettingsButton.setBounds (row.removeFromLeft (rowHeight));
    row.removeFromLeft (controlGap);

    quantizeTransientsToggle.setBounds (row.removeFromLeft (90));

    waveformDisplay.setBounds (pad, startY + pad + rowHeight + rowToWaveformGap, width - pad * 2, waveformHeight);

    return panelHeight;
}

void SlicerAudioProcessorEditor::showWaveformAdvancedPopover()
{
    auto content = std::make_unique<juce::Component>();

    constexpr int w = 280;
    constexpr int rowH = 26;
    constexpr int rowGap = 8;

    content->addAndMakeVisible (resetEditsButton);
    content->addAndMakeVisible (undoButton);
    content->addAndMakeVisible (redoButton);

    content->addAndMakeVisible (sensitivityLabel);
    content->addAndMakeVisible (sensitivityField);

    content->addAndMakeVisible (quantizeGridLabel);
    content->addAndMakeVisible (quantizeGridSelector);

    content->addAndMakeVisible (manualBpmOverrideToggle);
    content->addAndMakeVisible (manualBpmOverrideLabel);
    content->addAndMakeVisible (manualBpmOverrideSlider);

    const bool timeStretch = pitchModeSegments.getSelectedIndex() == 1;
    content->addAndMakeVisible (beatQuantizeToggleRepitch);
    content->addAndMakeVisible (pitchModeExtraViewport);

    updateQuantizeTransientsVisibility();
    updateManualBpmOverrideVisibility();
    updatePitchModeVisibility();

    juce::Rectangle<int> area (0, 0, w, 3000);

    {
        auto r = area.removeFromTop (rowH);
        resetEditsButton.setBounds (r.removeFromLeft (resetEditsButton.getBestWidthForHeight (rowH) + 16));
        r.removeFromLeft (6);
        undoButton.setBounds (r.removeFromLeft (75));
        r.removeFromLeft (6);
        redoButton.setBounds (r.removeFromLeft (75));
        area.removeFromTop (rowGap + 4);
    }

    {
        auto r = area.removeFromTop (rowH);
        sensitivityLabel.setBounds (r.removeFromLeft (130).withSizeKeepingCentre (130, 20));
        sensitivityField.setBounds (r.removeFromLeft (50));
        area.removeFromTop (rowGap);
    }

    if (quantizeTransientsToggle.getToggleState())
    {
        auto r = area.removeFromTop (rowH);
        quantizeGridLabel.setBounds (r.removeFromLeft (40));
        quantizeGridSelector.setBounds (r);
        area.removeFromTop (rowGap);
    }

    manualBpmOverrideToggle.setBounds (area.removeFromTop (24));
    area.removeFromTop (6);

    if (manualBpmOverrideToggle.getToggleState())
    {
        auto r = area.removeFromTop (rowH);
        manualBpmOverrideLabel.setBounds (r.removeFromLeft (40));
        manualBpmOverrideSlider.setBounds (r);
        area.removeFromTop (rowGap);
    }

    if (! timeStretch)
    {
        beatQuantizeToggleRepitch.setBounds (area.removeFromTop (24));
        area.removeFromTop (rowGap);
    }
    else
    {
        constexpr int extraViewportHeight = 150;
        pitchModeExtraViewport.setBounds (area.removeFromTop (extraViewportHeight));
        area.removeFromTop (rowGap);

        // pitchModeExtraContent's own internal row layout -- identical shape
        // to the old layoutUniversalControlsRow()'s inline block.
        const int extraWidth = pitchModeExtraViewport.getWidth() - pitchModeExtraViewport.getScrollBarThickness();
        juce::Rectangle<int> extraContent (0, 0, extraWidth, 4000);
        const int extraStartHeight = extraContent.getHeight();

        auto grainSizeRow = extraContent.removeFromTop (30);
        grainSizeLabel.setBounds (grainSizeRow.removeFromLeft (110));
        grainSizeSlider.setBounds (grainSizeRow);
        extraContent.removeFromTop (10);

        auto windowShapeRow = extraContent.removeFromTop (30);
        windowShapeLabel.setBounds (windowShapeRow.removeFromLeft (110));
        windowShapeSelector.setBounds (windowShapeRow.removeFromLeft (150));
        extraContent.removeFromTop (10);

        beatQuantizeToggle.setBounds (extraContent.removeFromTop (24));
        extraContent.removeFromTop (10);

        auto pitchShiftRow = extraContent.removeFromTop (30);
        pitchShiftLabel.setBounds (pitchShiftRow.removeFromLeft (110));
        pitchShiftSlider.setBounds (pitchShiftRow);

        const int extraConsumed = extraStartHeight - extraContent.getHeight();
        pitchModeExtraContent.setSize (extraWidth, extraConsumed);
    }

    content->setSize (w, 3000 - area.getHeight());
    content->setLookAndFeel (&neditLookAndFeel);

    const auto areaToPointTo = getLocalArea (&advancedSettingsButton, advancedSettingsButton.getLocalBounds());
    auto& box = juce::CallOutBox::launchAsynchronously (std::move (content), areaToPointTo, this);
    box.setLookAndFeel (&neditLookAndFeel);
}

int SlicerAudioProcessorEditor::layoutSubModeContent (int contentWidth)
{
    switch (subModeTabs.getSelectedIndex())
    {
        case 1: return layoutSequenceTab (contentWidth, 0);
        case 2: return layoutControlTab (contentWidth, 0);
        case 3: return layoutPerformTab (contentWidth, 0);
        default: return layoutGenerateTab (contentWidth, 0);
    }
}

void SlicerAudioProcessorEditor::updateWindowSize()
{
    const int contentWidth = minContentWidth;
    const int controlsTotalHeight = layoutControlsContent (contentWidth);

    // Reserved to exactly the ACTIVE tab's own real preferred height (not a
    // worst-case across all four) -- the window resizes on every tab
    // switch instead, so each tab's content fills its own space rather
    // than sitting in a void sized for a taller sibling tab. No tab ever
    // needs to scroll internally in practice; subModeViewport stays a
    // Viewport purely as a safety net (see this class's own doc comment).
    const int subModeContentWidth = contentWidth - subModeViewport.getScrollBarThickness();
    subModeViewportHeight = layoutSubModeContent (subModeContentWidth);
    subModeContent.setSize (subModeContentWidth, subModeViewportHeight);

    const int windowWidth = contentWidth + windowMargin * 2;
    const int windowHeight = windowMargin * 2 + headerTextHeight + controlsTotalHeight + layoutGap + subModeViewportHeight;

    setSize (windowWidth, windowHeight);
}

int SlicerAudioProcessorEditor::layoutGenerateTab (int contentWidth, int startY)
{
    constexpr int panelGap = 14;
    constexpr int sectionSacrificialHeight = 1200;
    juce::Rectangle<int> area (0, startY, contentWidth, 4000);

    playbackStyleSegments.setBounds (area.removeFromTop (SegmentedButtonRow::preferredHeight));
    area.removeFromTop (8);

    playbackStyleFaderRow.setBounds (area.removeFromTop (PlaybackStyleFaderRow::getPreferredHeight()));
    area.removeFromTop (10);

    playbackStyleParametersLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);

    // Fixed height regardless of which style is selected -- every style's
    // dials sit in one row now (PlaybackStyleParameterPanel::getPreferredHeight()),
    // so picking a different style never changes this panel's reserved
    // space, and in turn never resizes the window (see updateWindowSize()'s
    // own doc comment on this app's "fixed-height container sized to the
    // worst case" layout principle).
    constexpr int paramAreaPad = 10;
    const int paramPanelHeight = PlaybackStyleParameterPanel::getPreferredHeight();
    const int backdropHeight = paramPanelHeight + paramAreaPad * 2;

    generateParameterAreaBackdrop.setBounds (area.removeFromTop (backdropHeight));
    playbackStyleParameterPanel.setBounds (generateParameterAreaBackdrop.getX() + paramAreaPad,
                                            generateParameterAreaBackdrop.getY() + paramAreaPad,
                                            contentWidth - paramAreaPad * 2, paramPanelHeight);
    area.removeFromTop (panelGap);

    auto layoutSection = [&] (SectionPanel& panel, auto&& layoutFn)
    {
        panel.setBounds (area.getX(), area.getY(), area.getWidth(), sectionSacrificialHeight);
        auto content = sectionContentArea (panel);
        const int startHeight = content.getHeight();

        layoutFn (content);

        const int consumed = startHeight - content.getHeight();
        const int panelHeight = SectionPanel::titleBarHeight + consumed + 12;
        panel.setBounds (area.getX(), area.getY(), area.getWidth(), panelHeight);
        area.removeFromTop (panelHeight);
        area.removeFromTop (panelGap);
    };

    layoutSection (timingSectionPanel, [this] (juce::Rectangle<int>& content)
    {
        sliceLengthClockLabel.setBounds (content.removeFromTop (20));
        sliceLengthClockSegments.setBounds (content.removeFromTop (SegmentedButtonRow::preferredHeight));
        content.removeFromTop (10);

        // Fixed-height container: Clock's own controls (clock reference/
        // Tape Stop scope/Filter Sweep scope/the compact Subdivision fader
        // row) are always laid out here, regardless of which sub-mode is
        // actually selected, so this section's reserved height is always
        // Clock's full height -- the worst case between the two sub-modes
        // -- and never changes when the Slice Length/Clock toggle changes.
        // Slice Length's own (shorter) row is laid out separately from the
        // same starting point below, so it occupies the top portion of
        // this same fixed space rather than the space shrinking to fit it.
        // updateSliceLengthClockVisibility() hides whichever set doesn't
        // apply to the current selection; only the visible set is ever
        // interactive.
        const auto subModeTop = content;

        auto clockReferenceRow = content.removeFromTop (30);
        clockReferenceLabel.setBounds (clockReferenceRow.removeFromLeft (140));
        clockReferenceSelector.setBounds (clockReferenceRow.removeFromLeft (150));
        content.removeFromTop (10);

        auto tapeStopScopeRow = content.removeFromTop (30);
        tapeStopScopeLabel.setBounds (tapeStopScopeRow.removeFromLeft (140));
        tapeStopScopeSelector.setBounds (tapeStopScopeRow.removeFromLeft (150));
        content.removeFromTop (10);

        auto filterSweepScopeRow = content.removeFromTop (30);
        filterSweepScopeLabel.setBounds (filterSweepScopeRow.removeFromLeft (140));
        filterSweepScopeSelector.setBounds (filterSweepScopeRow.removeFromLeft (150));
        content.removeFromTop (10);

        subdivisionTableLabel.setBounds (content.removeFromTop (20));
        subdivisionGrid.setBounds (content.removeFromTop (SubdivisionProbabilityGrid::getPreferredHeight()));

        auto sliceLengthArea = subModeTop;
        auto resetEveryRow = sliceLengthArea.removeFromTop (30);
        resetEveryLabel.setBounds (resetEveryRow.removeFromLeft (140));
        resetEverySelector.setBounds (resetEveryRow.removeFromLeft (150));
    });

    return 4000 - area.getHeight();
}

int SlicerAudioProcessorEditor::layoutSequenceTab (int contentWidth, int startY)
{
    juce::Rectangle<int> area (0, startY, contentWidth, 4000);

    auto topRow = area.removeFromTop (28);

    const int swatchRowWidth = PlaybackStyleSwatchRow::getPreferredWidth();
    playbackStyleSwatchRow.setBounds (topRow.removeFromLeft (swatchRowWidth)
                                             .withSizeKeepingCentre (swatchRowWidth, PlaybackStyleSwatchRow::swatchSize));
    topRow.removeFromLeft (14);

    randomizeSequenceButton.setBounds (topRow.removeFromLeft (randomizeSequenceButton.getBestWidthForHeight (26) + 18));
    topRow.removeFromLeft (6);
    clearSequenceButton.setBounds (topRow.removeFromLeft (clearSequenceButton.getBestWidthForHeight (26) + 18));

    // Compact pattern length/step resolution readout, right-aligned --
    // each ComboBox's own selected text ("1 bar", "1/16") already shows the
    // value, no separate label row needed.
    auto rightGroup = topRow.removeFromRight (260);
    patternLengthSelector.setBounds (rightGroup.removeFromLeft (125));
    rightGroup.removeFromLeft (8);
    stepResolutionSelector.setBounds (rightGroup);

    area.removeFromTop (10);

    // Probability fader row -- restores the original spec's "Sequence
    // shows the probability table (Randomize reads from it)" (see
    // sequencePlaybackStyleFaderRow's own member doc comment for why this
    // is a separate PlaybackStyleFaderRow instance from Generate's).
    sequencePlaybackStyleFaderRow.setBounds (area.removeFromTop (PlaybackStyleFaderRow::getPreferredHeight()));
    area.removeFromTop (14);

    // sequencerGrid still gets the large majority of the tab's space.
    constexpr int sequencerRowHeight = 380;
    sequencerViewport.setBounds (area.removeFromTop (sequencerRowHeight));
    sequencerGrid.setTargetWidth (contentWidth - sequencerViewport.getScrollBarThickness());
    sequencerGrid.setAvailableHeight (sequencerViewport.getHeight());
    area.removeFromTop (16);

    // Pattern Switch Timing -- a compact row above the pattern bank's own
    // keyboard, now that the keyboard needs the full tab width rather than
    // a narrow sidebar column.
    auto switchTimingRow = area.removeFromTop (28);
    patternSwitchTimingLabel.setBounds (switchTimingRow.removeFromLeft (120));
    patternSwitchTimingSelector.setBounds (switchTimingRow.removeFromLeft (150));
    switchTimingRow.removeFromLeft (20);
    patternSwitchIntervalLabel.setBounds (switchTimingRow.removeFromLeft (120));
    patternSwitchIntervalSelector.setBounds (switchTimingRow.removeFromLeft (150));
    area.removeFromTop (10);

    patternBankPanel.setBounds (area.removeFromTop (PatternBankPanel::getPreferredHeight()));
    area.removeFromTop (10);

    return 4000 - area.getHeight();
}

int SlicerAudioProcessorEditor::layoutPerformTab (int contentWidth, int startY)
{
    juce::Rectangle<int> area (0, startY, contentWidth, 4000);

    performanceStyleParametersLabel.setBounds (area.removeFromTop (20));
    performanceStyleParameterPanel.setBounds (area.removeFromTop (PlaybackStyleParameterPanel::getPreferredHeight()));
    area.removeFromTop (20);

    auto performanceToggleRow = area.removeFromTop (24);
    performanceLoopToggle.setBounds (performanceToggleRow.removeFromLeft (100));
    performanceToggleRow.removeFromLeft (10);
    performanceSyncToggle.setBounds (performanceToggleRow.removeFromLeft (100));
    area.removeFromTop (10);

    auto performanceTrimSnapRow = area.removeFromTop (30);
    performanceTrimSnapLabel.setBounds (performanceTrimSnapRow.removeFromLeft (140));
    performanceTrimSnapSelector.setBounds (performanceTrimSnapRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceTrimGridRow = area.removeFromTop (30);
    performanceTrimGridLabel.setBounds (performanceTrimGridRow.removeFromLeft (140));
    performanceTrimGridSelector.setBounds (performanceTrimGridRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceQuantizeRecallRow = area.removeFromTop (24);
    performanceQuantizeRecallToggle.setBounds (performanceQuantizeRecallRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceQuantizeRecallIntervalRow = area.removeFromTop (30);
    performanceQuantizeRecallIntervalLabel.setBounds (performanceQuantizeRecallIntervalRow.removeFromLeft (140));
    performanceQuantizeRecallIntervalSelector.setBounds (performanceQuantizeRecallIntervalRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceKeyboardRow = area.removeFromTop (PerformanceKeyboardPanel::getPreferredHeight());
    performanceKeyboardPanel.setBounds (performanceKeyboardRow);

    return 4000 - area.getHeight();
}

int SlicerAudioProcessorEditor::layoutControlTab (int contentWidth, int startY)
{
    juce::Rectangle<int> area (0, startY, contentWidth, 4000);

    auto headerRow = area.removeFromTop (30);
    controlBaseNoteLabel.setBounds (headerRow.removeFromLeft (90));
    controlBaseNoteSlider.setBounds (headerRow.removeFromLeft (140));
    headerRow.removeFromLeft (10);
    controlBaseNoteNameLabel.setBounds (headerRow.removeFromLeft (60));
    headerRow.removeFromLeft (24);
    controlGateModeLabel.setBounds (headerRow.removeFromLeft (90));
    controlGateModeSegments.setBounds (headerRow.removeFromLeft (160));
    area.removeFromTop (6);

    controlSliceRangeLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (16);

    auto legendRow = area.removeFromTop (18);
    controlLegendKeyswitchLabel.setBounds (legendRow.removeFromLeft (160));
    controlLegendSliceLabel.setBounds (legendRow.removeFromLeft (80));
    area.removeFromTop (6);

    controlKeyboardPanel.setBounds (area.removeFromTop (ControlKeyboardPanel::getPreferredHeight()));
    area.removeFromTop (20);

    controlStyleParametersLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);
    controlStyleParameterPanel.setBounds (area.removeFromTop (PlaybackStyleParameterPanel::getPreferredHeight()));

    return 4000 - area.getHeight();
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
    else if (button == &randomizeSequenceButton)
    {
        processor.randomizeSequence();
        sequencerGrid.repaint(); // immediate feedback rather than waiting for the next 30fps timer tick
    }
    else if (button == &clearSequenceButton)
    {
        processor.clearSequence();
        sequencerGrid.repaint();
    }
}

void SlicerAudioProcessorEditor::timerCallback()
{
    undoButton.setEnabled (processor.canUndoEdit());
    redoButton.setEnabled (processor.canRedoEdit());

    const bool auditioning = processor.getAuditionActive();
    auditionButton.setButtonText (auditioning ? "Stop" : "Audition");
    auditionButton.setColour (juce::TextButton::buttonColourId,
                               auditioning ? juce::Colours::orange.withAlpha (0.6f)
                                           : getLookAndFeel().findColour (juce::TextButton::buttonColourId));

    // A MIDI pattern-bank recall can change stepResolutionIndex/
    // patternLengthBarsIndex on its own, out from under these two -- poll
    // and resync rather than let them show a stale value.
    const int processorStepResolutionId = processor.getStepResolutionIndex() + 1;
    if (stepResolutionSelector.getSelectedId() != processorStepResolutionId)
        stepResolutionSelector.setSelectedId (processorStepResolutionId, juce::dontSendNotification);

    const int processorPatternLengthId = processor.getPatternLengthBarsIndex() + 1;
    if (patternLengthSelector.getSelectedId() != processorPatternLengthId)
        patternLengthSelector.setSelectedId (processorPatternLengthId, juce::dontSendNotification);

    // Performance mode's working state -- same "poll and resync" reasoning:
    // a click on the on-screen keyboard loads a new slot straight into
    // performanceWorkingState from the keyboard panel's own click handler,
    // out from under this parameter panel/these toggles.
    performanceStyleParameterPanel.setSelectedStyle (processor.getPerformanceWorkingStyle());
    performanceStyleParameterPanel.repaint();

    const bool processorPerformanceLoop = processor.getPerformanceWorkingLoop();
    if (performanceLoopToggle.getToggleState() != processorPerformanceLoop)
        performanceLoopToggle.setToggleState (processorPerformanceLoop, juce::dontSendNotification);

    const bool processorPerformanceSync = processor.getPerformanceWorkingSync();
    if (performanceSyncToggle.getToggleState() != processorPerformanceSync)
        performanceSyncToggle.setToggleState (processorPerformanceSync, juce::dontSendNotification);
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

void SlicerAudioProcessorEditor::updateSliceLengthClockVisibility()
{
    const int selectedIndex = sliceLengthClockSegments.getSelectedIndex();
    const bool sliceLength = selectedIndex == 0;
    const bool clock = selectedIndex == 1;

    clockReferenceLabel.setVisible (clock);
    clockReferenceSelector.setVisible (clock);
    tapeStopScopeLabel.setVisible (clock);
    tapeStopScopeSelector.setVisible (clock);
    filterSweepScopeLabel.setVisible (clock);
    filterSweepScopeSelector.setVisible (clock);
    subdivisionTableLabel.setVisible (clock);
    subdivisionGrid.setVisible (clock);

    resetEveryLabel.setVisible (sliceLength);
    resetEverySelector.setVisible (sliceLength);
}

void SlicerAudioProcessorEditor::updatePitchModeVisibility()
{
    const int selectedIndex = pitchModeSegments.getSelectedIndex();
    const bool repitch = selectedIndex == 0;
    const bool timeStretch = selectedIndex == 1;

    beatQuantizeToggleRepitch.setVisible (repitch);

    pitchModeExtraViewport.setVisible (timeStretch);
    grainSizeLabel.setVisible (timeStretch);
    grainSizeSlider.setVisible (timeStretch);
    windowShapeLabel.setVisible (timeStretch);
    windowShapeSelector.setVisible (timeStretch);
    beatQuantizeToggle.setVisible (timeStretch);
    pitchShiftLabel.setVisible (timeStretch);
    pitchShiftSlider.setVisible (timeStretch);
}

void SlicerAudioProcessorEditor::updateManualBpmOverrideVisibility()
{
    const bool enabled = manualBpmOverrideToggle.getToggleState();

    manualBpmOverrideLabel.setVisible (enabled);
    manualBpmOverrideSlider.setVisible (enabled);
}

void SlicerAudioProcessorEditor::updateQuantizeTransientsVisibility()
{
    const bool enabled = quantizeTransientsToggle.getToggleState();

    quantizeGridLabel.setVisible (enabled);
    quantizeGridSelector.setVisible (enabled);
}

void SlicerAudioProcessorEditor::updatePerformanceTrimSnapVisibility()
{
    const bool performance = processor.getTriggerMode() == SlicerAudioProcessor::TriggerMode::performance;
    const bool grid = performanceTrimSnapSelector.getSelectedId() == 2;

    performanceTrimGridLabel.setVisible (performance && grid);
    performanceTrimGridSelector.setVisible (performance && grid);
}

void SlicerAudioProcessorEditor::updatePerformanceQuantizeRecallVisibility()
{
    const bool performance = processor.getTriggerMode() == SlicerAudioProcessor::TriggerMode::performance;
    const bool quantized = performanceQuantizeRecallToggle.getToggleState();

    performanceQuantizeRecallIntervalLabel.setVisible (performance && quantized);
    performanceQuantizeRecallIntervalSelector.setVisible (performance && quantized);
}

void SlicerAudioProcessorEditor::updateControlBaseNoteDisplay()
{
    const int baseNote = processor.getControlBaseNote();
    controlBaseNoteNameLabel.setText (juce::MidiMessage::getMidiNoteName (baseNote, true, true, 3),
                                       juce::dontSendNotification);

    const int numSlices = processor.getSequencerNumRows();

    if (numSlices <= 0)
    {
        controlSliceRangeLabel.setText ("No slices detected yet", juce::dontSendNotification);
    }
    else
    {
        const int topNote = juce::jmin (127, baseNote + numSlices - 1);
        controlSliceRangeLabel.setText (
            "Slices: " + juce::MidiMessage::getMidiNoteName (baseNote, true, true, 3) + " - "
                + juce::MidiMessage::getMidiNoteName (topNote, true, true, 3)
                + " (" + juce::String (numSlices) + ")",
            juce::dontSendNotification);
    }

    controlKeyboardPanel.refresh();
}

void SlicerAudioProcessorEditor::updateControlKeyboardPanel()
{
    controlKeyboardPanel.refresh();
}

void SlicerAudioProcessorEditor::updateControlStyleParameterPanelVisibility()
{
    const bool showPanel = subModeTabs.getSelectedIndex() == 2 && controlSelectedKeyswitchStyle >= 0;
    controlStyleParametersLabel.setVisible (showPanel);
    controlStyleParameterPanel.setVisible (showPanel);
}

void SlicerAudioProcessorEditor::onControlKeyswitchSelected (int styleIndex)
{
    controlSelectedKeyswitchStyle = styleIndex;
    controlStyleParameterPanel.setSelectedStyle (styleIndex);
    updateControlStyleParameterPanelVisibility();
    controlKeyboardPanel.repaint();
}

void SlicerAudioProcessorEditor::onGenerateStyleSelected (int styleIndex)
{
    playbackStyleParameterPanel.setSelectedStyle (styleIndex);
    playbackStyleParameterPanel.repaint();
    playbackStyleFaderRow.setSelectedStyle (styleIndex);

    // No updateWindowSize() call here -- the parameter panel's reserved
    // height no longer depends on which style is selected (see its own
    // getPreferredHeight() doc comment), so picking a style never needs to
    // resize the window.
}

void SlicerAudioProcessorEditor::updateActiveTabVisibility()
{
    const int subIndex = subModeTabs.getSelectedIndex();
    const bool generateActive = subIndex == 0;
    const bool sequenceActive = subIndex == 1;
    const bool controlActive = subIndex == 2;
    const bool performActive = subIndex == 3;

    for (auto* c : generateComponents)
        c->setVisible (generateActive);

    if (generateActive)
        updateSliceLengthClockVisibility();

    for (auto* c : sequenceComponents)
        c->setVisible (sequenceActive);

    if (sequenceActive)
    {
        const bool setInterval = patternSwitchTimingSelector.getSelectedId()
            == static_cast<int> (SlicerAudioProcessor::PatternSwitchTiming::setInterval) + 1;
        patternSwitchIntervalLabel.setVisible (setInterval);
        patternSwitchIntervalSelector.setVisible (setInterval);
    }

    for (auto* c : performComponents)
        c->setVisible (performActive);

    if (performActive)
    {
        updatePerformanceTrimSnapVisibility();
        updatePerformanceQuantizeRecallVisibility();
    }

    for (auto* c : controlComponents)
        c->setVisible (controlActive);

    if (controlActive)
    {
        updateControlBaseNoteDisplay();
        updateControlKeyboardPanel();
    }

    updateControlStyleParameterPanelVisibility(); // hides the panel outright when Control isn't the active tab
}

void SlicerAudioProcessorEditor::syncTriggerModeToActiveTab()
{
    using TM = SlicerAudioProcessor::TriggerMode;

    const TM desired = subModeTabs.getSelectedIndex() == 1 ? TM::sequenced    // Sequence tab
                      : subModeTabs.getSelectedIndex() == 2 ? TM::control     // Control tab
                      : subModeTabs.getSelectedIndex() == 3 ? TM::performance // Perform tab
                                                             : lastGenerateTriggerMode; // Generate

    if (processor.getTriggerMode() == desired)
        return; // guard: setTriggerMode() unconditionally resets clock/reset/sequenced/performance init flags even for a no-op mode

    const bool wasPerformance = processor.getTriggerMode() == TM::performance;
    processor.setTriggerMode (desired);

    // Leaving Performance mode: Performance's own focus-change path
    // deliberately raw-stores trim, so `slices` can be stale against
    // wherever Performance mode left the trim. Force one rebuild here by
    // re-invoking the existing public trim setter with the same value it
    // already has, purely for its rebuild side effect.
    if (wasPerformance)
        processor.setTrimEndSample (processor.getTrimEndSample(), false);

    if (subModeTabs.getSelectedIndex() == 0)
        updateSliceLengthClockVisibility();
}

void SlicerAudioProcessorEditor::updateAfterSampleOrSliceChange()
{
    const int numSlices = processor.getNumSlices();
    const juce::String text = processor.getLoadedFileName()
                             + "  -  " + juce::String (numSlices)
                             + " slice" + (numSlices == 1 ? "" : "s");
    statusLabel.setText (text, juce::dontSendNotification);

    const double bpm = processor.getCalculatedOriginalBpm();
    calculatedBpmLabel.setText (bpm > 0.0 ? ("~" + juce::String (bpm, 1) + " BPM") : "",
                                 juce::dontSendNotification);

    updateControlBaseNoteDisplay(); // Control mode's slice-range readout depends on the current slice count
    waveformDisplay.refresh();
}
