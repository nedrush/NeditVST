#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // Pass 3 window-sizing constants -- shared between the constructor's
    // one-time size computation and resized()'s own layout, so the two
    // never drift out of sync with each other.
    constexpr int windowMargin = 20;         // matches getLocalBounds().reduced (windowMargin) below
    constexpr int headerTextHeight = 30;     // space for paint()'s header text
    constexpr int layoutGap = 14;            // vertical/horizontal gap between the universal layer/Playback Style/sub-mode tab row/Layer 5 sections
    constexpr int controlsToZoomGap = 20;
    constexpr int zoomRowHeight = 30;
    constexpr int zoomToWaveformGap = 10;
    constexpr int minWaveformHeight = 190;   // Pass 3: the window now grows to guarantee this rather than squeezing whatever space is left
    constexpr int minContentWidth = 1000;    // floor so the sub-mode tab row/segmented rows/sequencer grid have comfortable room even if Layer 1's own natural width comes in narrower
}

SlicerAudioProcessorEditor::SlicerAudioProcessorEditor (SlicerAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p),
      playbackStyleGrid (p), playbackStyleParameterPanel (p), subdivisionGrid (p), playbackStylePalette (p),
      sequencerBankSource (p), patternBankPanel (sequencerBankSource),
      performanceStyleParameterPanel (p,
          [&p] (int i) { return p.getPerformanceWorkingParameterValue (i); },
          [&p] (int i, float v) { p.setPerformanceWorkingParameterValue (i, v); },
          p.getPerformanceWorkingStyle() + 1,
          [&p] (int style) { p.setPerformanceWorkingStyle (style); }),
      performanceKeyboardSource (p), performanceKeyboardPanel (performanceKeyboardSource),
      sequencerGrid (p), waveformDisplay (p)
{
    addAndMakeVisible (controlsContent); // Pass 3: Layers 1-4, plain non-scrolling child
    controlsContent.setLookAndFeel (&neditLookAndFeel); // Tungsten/Salmon palette for every native-widget control below (Pass 1)

    // Layer 5 (Pass 3) -- the one deliberate scrolling region left in this
    // editor (see subModeViewport's own doc comment in the header). A
    // separate Component tree from controlsContent, so it needs its own
    // LookAndFeel scoping too.
    addAndMakeVisible (subModeViewport);
    subModeViewport.setViewedComponent (&subModeContent, false); // we own it, don't let the viewport delete it
    subModeViewport.setScrollBarsShown (true, false); // vertical only, shown when needed
    subModeContent.setLookAndFeel (&neditLookAndFeel);

    // Sub-mode tab row -- added first so every page/section beneath it
    // paints on top (SectionPanel is a pure backdrop, added here purely for
    // correct paint z-order, not for layout).
    controlsContent.addAndMakeVisible (subModeTabs);
    subModeTabs.setOptions ({ { "Generate", std::nullopt }, { "Sequence", std::nullopt },
                               { "Control", std::nullopt }, { "Perform", std::nullopt } });

    // Seed both tab rows from the processor's own current trigger mode so
    // the initial syncTriggerModeToActiveTab() call below is a no-op --
    // e.g. if the processor is already in Performance mode (persisted
    // state), land on Beats>Perform at startup rather than silently
    // resetting it back to Slice Length.
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

    subModeTabs.onSelectionChanged = [this] (int) { updateActiveTabVisibility(); syncTriggerModeToActiveTab(); resized(); };

    controlsContent.addAndMakeVisible (sampleSectionPanel);
    controlsContent.addAndMakeVisible (trimTempoSectionPanel);
    controlsContent.addAndMakeVisible (detectionSectionPanel);
    controlsContent.addAndMakeVisible (fadeSectionPanel);
    controlsContent.addAndMakeVisible (pitchModeSectionPanel);
    controlsContent.addAndMakeVisible (playbackStyleSectionPanel);
    subModeContent.addAndMakeVisible (timingSectionPanel);

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
    loopLengthSlider.setScrollWheelEnabled (false); // a holdover from when this editor scrolled internally; harmless now that it doesn't
    loopLengthSlider.setRange (1.0, 8.0, 1.0);
    loopLengthSlider.setNumDecimalPlacesToDisplay (0);
    loopLengthSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 20);
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
    manualBpmOverrideSlider.setScrollWheelEnabled (false); // a holdover from when this editor scrolled internally; harmless now that it doesn't
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

        // Pitch Mode's own panel height is mode-aware now (Pass 5) -- the
        // window has to follow it.
        updateWindowSize();
    };

    controlsContent.addAndMakeVisible (beatQuantizeToggleRepitch);
    beatQuantizeToggleRepitch.setToggleState (processor.getBeatQuantizeSliceLengthEnabledRepitch(), juce::dontSendNotification);
    beatQuantizeToggleRepitch.onClick = [this]
    {
        processor.setBeatQuantizeSliceLengthEnabledRepitch (beatQuantizeToggleRepitch.getToggleState());
    };

    // Pass 3 -- viewed through pitchModeExtraViewport (see its own doc
    // comment in the header), a separate Component tree from controlsContent
    // needing its own LookAndFeel scoping too.
    controlsContent.addAndMakeVisible (pitchModeExtraViewport);
    pitchModeExtraViewport.setViewedComponent (&pitchModeExtraContent, false); // we own it, don't let the viewport delete it
    pitchModeExtraViewport.setScrollBarsShown (true, false); // vertical only, shown when needed
    pitchModeExtraContent.setLookAndFeel (&neditLookAndFeel);

    pitchModeExtraContent.addAndMakeVisible (grainSizeLabel);
    grainSizeLabel.setText ("Grain size (ms)", juce::dontSendNotification);
    grainSizeLabel.setJustificationType (juce::Justification::centredLeft);

    pitchModeExtraContent.addAndMakeVisible (grainSizeSlider);
    grainSizeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    grainSizeSlider.setScrollWheelEnabled (false); // a holdover from when this editor scrolled internally; harmless now that it doesn't
    grainSizeSlider.setRange (20.0, 150.0, 1.0);
    grainSizeSlider.setValue (processor.getGrainSizeMs(), juce::dontSendNotification);
    grainSizeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    grainSizeSlider.onValueChange = [this]
    {
        processor.setGrainSizeMs ((float) grainSizeSlider.getValue());
    };

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
    pitchShiftSlider.setScrollWheelEnabled (false); // a holdover from when this editor scrolled internally; harmless now that it doesn't
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
    sensitivitySlider.setSliderStyle (juce::Slider::IncDecButtons);
    sensitivitySlider.setScrollWheelEnabled (false); // a holdover from when this editor scrolled internally; harmless now that it doesn't
    sensitivitySlider.setRange (0.0, 1.0, 0.01);
    sensitivitySlider.setNumDecimalPlacesToDisplay (2);
    sensitivitySlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 20);
    sensitivitySlider.setValue (processor.getSensitivity(), juce::dontSendNotification);
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
        // or for non-drag changes.
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

    controlsContent.addAndMakeVisible (quantizeTransientsToggle);
    quantizeTransientsToggle.setToggleState (processor.getQuantizeTransientsEnabled(), juce::dontSendNotification);
    quantizeTransientsToggle.onClick = [this]
    {
        processor.setQuantizeTransientsEnabled (quantizeTransientsToggle.getToggleState());
        updateQuantizeTransientsVisibility();
        updateAfterSampleOrSliceChange();
    };

    controlsContent.addAndMakeVisible (quantizeGridLabel);
    quantizeGridLabel.setText ("Grid", juce::dontSendNotification);
    quantizeGridLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (quantizeGridSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        quantizeGridSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    quantizeGridSelector.setSelectedId (processor.getQuantizeGridIndex() + 1, juce::dontSendNotification);
    quantizeGridSelector.onChange = [this]
    {
        processor.setQuantizeGridIndex (quantizeGridSelector.getSelectedId() - 1);
        updateAfterSampleOrSliceChange();
    };

    controlsContent.addAndMakeVisible (fadeInLabel);
    fadeInLabel.setText ("Fade in (ms)", juce::dontSendNotification);
    fadeInLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (fadeInSlider);
    fadeInSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    fadeInSlider.setScrollWheelEnabled (false); // a holdover from when this editor scrolled internally; harmless now that it doesn't
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
    fadeOutSlider.setScrollWheelEnabled (false); // a holdover from when this editor scrolled internally; harmless now that it doesn't
    fadeOutSlider.setRange (0.0, 100.0, 0.5);
    fadeOutSlider.setValue (processor.getFadeOutMs(), juce::dontSendNotification);
    fadeOutSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    fadeOutSlider.onValueChange = [this]
    {
        processor.setFadeOutMs ((float) fadeOutSlider.getValue());
    };

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
    };

    controlsContent.addAndMakeVisible (playbackStyleSegments);
    {
        std::vector<SegmentedButtonRow::Option> styleOptions;
        for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
            styleOptions.push_back ({ SlicerAudioProcessor::getPlaybackStyleName (i), PlaybackStylePalette::getStyleColour (i) });
        playbackStyleSegments.setOptions (std::move (styleOptions));
    }
    playbackStyleSegments.onSelectionChanged = [this] (int selectedIndex)
    {
        playbackStyleParameterPanel.setSelectedStyle (selectedIndex);
        playbackStyleParameterPanel.repaint();

        // The parameter viewport's own reserved height is per-style now
        // (Pass 5) -- the window has to follow it.
        updateWindowSize();
    };

    controlsContent.addAndMakeVisible (playbackStyleLabel);
    playbackStyleLabel.setText ("Playback style", juce::dontSendNotification);
    playbackStyleLabel.setJustificationType (juce::Justification::centredLeft);

    controlsContent.addAndMakeVisible (playbackStyleGrid);

    controlsContent.addAndMakeVisible (playbackStyleParametersLabel);
    playbackStyleParametersLabel.setText ("Style parameters", juce::dontSendNotification);
    playbackStyleParametersLabel.setJustificationType (juce::Justification::centredLeft);

    // Pass 3 -- viewed through a small fixed-height Viewport rather than
    // laid out inline at its own worst-case getPreferredHeight() (see its
    // member doc comment in the header for why).
    controlsContent.addAndMakeVisible (playbackStyleParameterViewport);
    playbackStyleParameterViewport.setViewedComponent (&playbackStyleParameterPanel, false); // we own it, don't let the viewport delete it
    playbackStyleParameterViewport.setScrollBarsShown (true, false); // vertical only, shown when needed
    // playbackStyleSegments above is the real selector now -- hide the
    // panel's own internal style ComboBox so the two don't sit redundantly
    // on top of each other (see setStyleSelectorVisible()'s own doc comment).
    playbackStyleParameterPanel.setStyleSelectorVisible (false);

    subModeContent.addAndMakeVisible (clockReferenceLabel);
    clockReferenceLabel.setText ("Clock reference", juce::dontSendNotification);
    clockReferenceLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (clockReferenceSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        clockReferenceSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
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
        tapeStopScopeSelector.addItem (SlicerAudioProcessor::getTapeStopScopeName (i), i + 1); // JUCE item IDs are 1-based
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
        filterSweepScopeSelector.addItem (SlicerAudioProcessor::getFilterSweepScopeName (i), i + 1); // JUCE item IDs are 1-based
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
        resetEverySelector.addItem (SlicerAudioProcessor::getResetBarsName (i), i + 1); // JUCE item IDs are 1-based
    resetEverySelector.setSelectedId (processor.getResetBarsIndex() + 1, juce::dontSendNotification);
    resetEverySelector.onChange = [this]
    {
        processor.setResetBarsIndex (resetEverySelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (subdivisionTableLabel);
    subdivisionTableLabel.setText ("Subdivision probability", juce::dontSendNotification);
    subdivisionTableLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (subdivisionGrid);

    subModeContent.addAndMakeVisible (stepResolutionLabel);
    stepResolutionLabel.setText ("Step resolution", juce::dontSendNotification);
    stepResolutionLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (stepResolutionSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        stepResolutionSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    stepResolutionSelector.setSelectedId (processor.getStepResolutionIndex() + 1, juce::dontSendNotification);
    stepResolutionSelector.onChange = [this]
    {
        processor.setStepResolutionIndex (stepResolutionSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (patternLengthLabel);
    patternLengthLabel.setText ("Pattern length (bars)", juce::dontSendNotification);
    patternLengthLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (patternLengthSelector);
    for (int i = 0; i < SlicerAudioProcessor::numPatternLengthBarsOptions; ++i)
        patternLengthSelector.addItem (SlicerAudioProcessor::getPatternLengthBarsName (i), i + 1); // JUCE item IDs are 1-based
    patternLengthSelector.setSelectedId (processor.getPatternLengthBarsIndex() + 1, juce::dontSendNotification);
    patternLengthSelector.onChange = [this]
    {
        processor.setPatternLengthBarsIndex (patternLengthSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (randomizeSequenceButton);
    randomizeSequenceButton.addListener (this);

    subModeContent.addAndMakeVisible (clearSequenceButton);
    clearSequenceButton.addListener (this);

    subModeContent.addAndMakeVisible (playbackStylePalette);
    subModeContent.addAndMakeVisible (patternBankPanel);

    // Pattern Switch Timing (Pass 2) -- governs when a pattern-bank recall
    // note-on (see patternBankPanel just above) actually takes effect.
    // Item IDs are 1-based, in PatternSwitchTiming enum order.
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
        updateActiveTabVisibility(); // Set Interval's own note-value picker only shows for that one timing mode (Sequence tab)
    };

    // Set Interval's grid point -- same note-value palette as Clock
    // reference/Step resolution above, shown only while Set Interval is
    // the selected timing (see updateActiveTabVisibility()).
    subModeContent.addAndMakeVisible (patternSwitchIntervalLabel);
    patternSwitchIntervalLabel.setText ("Switch interval", juce::dontSendNotification);
    patternSwitchIntervalLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (patternSwitchIntervalSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        patternSwitchIntervalSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    patternSwitchIntervalSelector.setSelectedId (processor.getPatternSwitchIntervalIndex() + 1, juce::dontSendNotification);
    patternSwitchIntervalSelector.onChange = [this]
    {
        processor.setPatternSwitchIntervalIndex (patternSwitchIntervalSelector.getSelectedId() - 1);
    };

    // Performance mode (Pass 1) -- style/params panel points at Performance
    // mode's own working-state storage (see the constructor init list
    // above), never the global default values Slice Length/Clock use.
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

    // Trim Snap mode -- Transients (existing behaviour) / Grid (fixed
    // musical grid at the established tempo). Item IDs mirror
    // SlicerAudioProcessor::TrimSnapMode's declaration order (1 = transients,
    // 2 = grid), same "JUCE item IDs are 1-based" convention as every other
    // enum-backed selector here.
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

    // Grid resolution -- same note-value palette as Clock reference/
    // Quantize Transients' Grid/Subdivide, shown only while Grid is the
    // selected snap mode (see updatePerformanceTrimSnapVisibility()).
    subModeContent.addAndMakeVisible (performanceTrimGridLabel);
    performanceTrimGridLabel.setText ("Grid", juce::dontSendNotification);
    performanceTrimGridLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (performanceTrimGridSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        performanceTrimGridSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    performanceTrimGridSelector.setSelectedId (processor.getPerformanceTrimGridIndex() + 1, juce::dontSendNotification);
    performanceTrimGridSelector.onChange = [this]
    {
        processor.setPerformanceTrimGridIndex (performanceTrimGridSelector.getSelectedId() - 1);
    };

    // Quantize Recall -- off (immediate, unchanged) by default, same
    // "preserve existing behaviour until explicitly opted into" convention
    // as Trim Snap's own toggle above.
    subModeContent.addAndMakeVisible (performanceQuantizeRecallToggle);
    performanceQuantizeRecallToggle.setToggleState (processor.getPerformanceQuantizeRecallEnabled(), juce::dontSendNotification);
    performanceQuantizeRecallToggle.onClick = [this]
    {
        processor.setPerformanceQuantizeRecallEnabled (performanceQuantizeRecallToggle.getToggleState());
        updatePerformanceQuantizeRecallVisibility();
    };

    // Quantize Recall's grid point -- same note-value palette as Clock
    // reference/Set Interval/Trim Snap's own Grid picker above, shown only
    // while the toggle is on (see updatePerformanceQuantizeRecallVisibility()).
    subModeContent.addAndMakeVisible (performanceQuantizeRecallIntervalLabel);
    performanceQuantizeRecallIntervalLabel.setText ("Recall interval", juce::dontSendNotification);
    performanceQuantizeRecallIntervalLabel.setJustificationType (juce::Justification::centredLeft);

    subModeContent.addAndMakeVisible (performanceQuantizeRecallIntervalSelector);
    for (int i = 0; i < SlicerAudioProcessor::numNoteValueOptions; ++i)
        performanceQuantizeRecallIntervalSelector.addItem (SlicerAudioProcessor::getNoteValueName (i), i + 1); // JUCE item IDs are 1-based
    performanceQuantizeRecallIntervalSelector.setSelectedId (processor.getPerformanceQuantizeRecallIntervalIndex() + 1, juce::dontSendNotification);
    performanceQuantizeRecallIntervalSelector.onChange = [this]
    {
        processor.setPerformanceQuantizeRecallIntervalIndex (performanceQuantizeRecallIntervalSelector.getSelectedId() - 1);
    };

    subModeContent.addAndMakeVisible (performanceKeyboardPanel);

    //=== Control mode ===
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
        updateControlBaseNoteDisplay();
        updateControlKeyswitchRows(); // keyswitch notes are computed straight from the base note -- moving it shifts the whole block
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

    subModeContent.addAndMakeVisible (controlKeyswitchSectionLabel);
    controlKeyswitchSectionLabel.setText ("Keyswitches (fixed, below the base note)", juce::dontSendNotification);
    controlKeyswitchSectionLabel.setJustificationType (juce::Justification::centredLeft);

    for (auto& label : controlKeyswitchLabels)
    {
        subModeContent.addAndMakeVisible (label);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    }

    updateControlBaseNoteDisplay();
    updateControlKeyswitchRows();

    subModeContent.addAndMakeVisible (sequencerViewport);
    sequencerViewport.setViewedComponent (&sequencerGrid, false); // we own it, don't let the viewport delete it
    sequencerViewport.setScrollBarsShown (true, true);

    // Pass 6 single universal layer -- every component that's unconditionally
    // visible regardless of the active sub-mode tab (Sample, plus what used
    // to be the separate Beats-specific block: Reset Edits+Undo+Redo, Tempo,
    // Detection, Fade In/Out, Pitch Mode), for updateActiveTabVisibility()'s
    // blanket show (see its own doc comment). Playback Style's own
    // components are deliberately NOT here -- their visibility is
    // mode-specific (Generate/Sequence only), set directly in
    // updateActiveTabVisibility(). Populated here, now that every control
    // referenced below is fully constructed.
    universalComponents = {
        &sampleSectionPanel, &loadButton, &statusLabel,
        &trimTempoSectionPanel, &detectionSectionPanel, &fadeSectionPanel, &pitchModeSectionPanel,
        &resetEditsButton, &undoButton, &redoButton,
        &auditionButton, &loopLengthLabel, &loopLengthSlider, &calculatedBpmLabel,
        &manualBpmOverrideToggle, &manualBpmOverrideLabel, &manualBpmOverrideSlider,
        &fadeInLabel, &fadeInSlider, &fadeOutLabel, &fadeOutSlider,
        &sensitivityLabel, &sensitivitySlider, &quantizeTransientsToggle, &quantizeGridLabel, &quantizeGridSelector,
        &pitchModeLabel, &pitchModeSegments, &beatQuantizeToggleRepitch, &pitchModeExtraViewport
    };

    generateComponents = {
        &timingSectionPanel, &sliceLengthClockLabel, &sliceLengthClockSegments,
        &clockReferenceLabel, &clockReferenceSelector, &tapeStopScopeLabel, &tapeStopScopeSelector,
        &filterSweepScopeLabel, &filterSweepScopeSelector, &resetEveryLabel, &resetEverySelector,
        &subdivisionTableLabel, &subdivisionGrid
    };

    sequenceComponents = {
        &stepResolutionLabel, &stepResolutionSelector, &patternLengthLabel, &patternLengthSelector,
        &randomizeSequenceButton, &clearSequenceButton, &playbackStylePalette, &sequencerViewport, &patternBankPanel,
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
        &controlGateModeLabel, &controlGateModeSegments, &controlKeyswitchSectionLabel
    };

    for (auto& label : controlKeyswitchLabels) controlComponents.push_back (&label);

    syncTriggerModeToActiveTab(); // no-op: both tab rows were already seeded from processor.getTriggerMode() above
    updateActiveTabVisibility(); // also drives updateSliceLengthClockVisibility()/updatePitchModeVisibility()/updateManualBpmOverrideVisibility()/updateQuantizeTransientsVisibility() for whichever tab is active

    // Zoom/pan (Step 31) — live directly on the editor, not controlsContent,
    // staying visually adjacent to the waveform they control.
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

    // Pass 5 -- initial sizing pass, now that every control exists and every
    // tab row is seeded/visible. updateWindowSize() (see its own doc comment)
    // does the actual measuring/setSize() and is also the same function
    // called later whenever Pitch Mode or Playback Style's own selection
    // changes -- this is just its first call, not special-cased logic of
    // its own.
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
    g.drawFittedText ("NeditVST - step 46: Filter Type, Curve Shape, Stretch Grain step editing",
                       getLocalBounds().removeFromTop (30), juce::Justification::centred, 1);

    // Loop Length staleness highlight (Step 33). loopLengthLabel/Knob live
    // inside controlsContent -- rather than computing their position by
    // hand, getLocalArea() walks the whole parent chain to get their real
    // on-screen rectangle in THIS component's coordinate space, so the
    // highlight tracks correctly regardless of how deeply nested inside a
    // SectionPanel's visual area the knob ends up (getLocalArea() doesn't
    // care, only real bounds
    // matter -- see SectionPanel's own class doc comment). Loop Length is
    // part of the single universal layer (Pass 6) -- always visible
    // regardless of the active sub-mode tab, so the isVisible() guard below
    // is really just a defensive check now. No clip region needed: with
    // controlsViewport gone, loopLengthLabel/Slider are always either fully
    // on-screen or not, never scrolled partway out of view.
    if (loopLengthNeedsAttention && loopLengthLabel.isVisible())
    {
        const auto labelBounds = getLocalArea (&loopLengthLabel, loopLengthLabel.getLocalBounds());
        const auto sliderBounds = getLocalArea (&loopLengthSlider, loopLengthSlider.getLocalBounds());
        const auto highlightBounds = labelBounds.getUnion (sliderBounds).expanded (4);

        g.setColour (juce::Colours::orange);
        g.drawRect (highlightBounds, 2);
    }
}

void SlicerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (windowMargin);
    area.removeFromTop (headerTextHeight); // space for the paint() header text

    // Layers 1-4 -- always fully visible, no Viewport, sized to exactly
    // whatever layoutControlsContent() reports (its own doc comment).
    const int contentWidth = area.getWidth();
    const int contentHeight = layoutControlsContent (contentWidth);
    controlsContent.setBounds (area.removeFromTop (contentHeight));
    area.removeFromTop (layoutGap);

    // Layer 5 -- the one deliberate scrolling region (subModeViewport's own
    // doc comment). Fixed VISIBLE height; subModeContent itself is sized to
    // whatever the active tab actually needs, scrolling internally when
    // that exceeds subModeViewportHeight.
    subModeViewport.setBounds (area.removeFromTop (subModeViewportHeight));
    area.removeFromTop (controlsToZoomGap);

    const int subModeContentWidth = subModeViewport.getWidth() - subModeViewport.getScrollBarThickness();
    const int subModeHeight = layoutSubModeContent (subModeContentWidth);
    subModeContent.setSize (subModeContentWidth, subModeHeight);

    // Zoom/pan (Step 31) — a small fixed row above the waveform, always
    // visible alongside it.
    auto zoomButtonsRow = area.removeFromTop (zoomRowHeight);
    zoomToTrimsButton.setBounds (zoomButtonsRow.removeFromLeft (150));
    zoomButtonsRow.removeFromLeft (10);
    resetZoomButton.setBounds (zoomButtonsRow.removeFromLeft (150));
    area.removeFromTop (zoomToWaveformGap);

    // SequencerGrid's own width (Step 38) -- driven by subModeContentWidth,
    // the SAME width basis layoutSubModeContent()/the palette row above
    // already used. Reduced by the Style Palette's own width + the gap
    // beside it (Step 41), so the COMBINED [palette][grid] row still
    // matches subModeContentWidth overall, same as the grid alone did
    // before the palette existed.
    sequencerGrid.setTargetWidth (subModeContentWidth - PlaybackStylePalette::preferredWidth - 10);

    waveformDisplay.setBounds (area); // takes up all remaining space, always fully visible
}

juce::Rectangle<int> SlicerAudioProcessorEditor::sectionContentArea (const SectionPanel& panel)
{
    return panel.getBounds().withTrimmedTop (SectionPanel::titleBarHeight).reduced (8, 6);
}

int SlicerAudioProcessorEditor::layoutControlsContent (int contentWidth)
{
    // Pass 6 -- the single universal layer (Sample via layoutTopToolbar(),
    // then Reset Edits+Undo+Redo/Tempo/Detection/Fade/Pitch Mode via
    // layoutUniversalControlsRow()) followed by the mode-specific Playback
    // Style area (layoutPlaybackStyleSection()) and the sub-mode tab row.
    // All of it is ALWAYS laid out regardless of which sub-mode tab is
    // active -- only Playback Style's own VISIBILITY (and so, indirectly,
    // the height it reports) varies with the active tab; see
    // updateActiveTabVisibility().
    int y = layoutTopToolbar (0);
    y += layoutGap;

    const int universalControlsHeight = layoutUniversalControlsRow (y);
    y += universalControlsHeight + layoutGap;

    const int playbackStyleHeight = layoutPlaybackStyleSection (contentWidth, y);
    y += playbackStyleHeight + layoutGap;

    subModeTabs.setBounds (0, y, contentWidth, SegmentedButtonRow::preferredHeight);
    y += SegmentedButtonRow::preferredHeight;

    return y;
}

int SlicerAudioProcessorEditor::layoutSubModeContent (int contentWidth)
{
    // Layer 5 -- whichever of Generate/Sequence/Control/Perform is actually
    // active, laid out inside subModeContent starting at y == 0
    // (subModeViewport handles scroll position, not this function).
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
    // Pass 5 -- extracted from what used to be the constructor's own
    // one-time measurement pass (Pass 4), now reusable: Pitch Mode's and
    // Playback Style's own panel heights vary by mode/style (see
    // layoutUniversalControlsRow()/layoutPlaybackStyleSection()), so this has
    // to be able to run again after construction too, not just once.
    //
    // The universal controls row's own self-sizing cluster (Reset Edits+Undo+
    // Redo/Tempo/Detection/Fade/Pitch Mode) doesn't depend on contentWidth --
    // it's laid out in two explicit rows of content-sized panels/clusters
    // (see layoutUniversalControlsRow()), and its own natural width is what
    // DEFINES contentWidth for every layer, not the other way around (Layer
    // 1's own Sample panel is much narrower and can't anchor this on its
    // own). Laying it out at startY == 0 here is the same call
    // layoutControlsContent() itself makes every frame -- not a separate
    // throwaway measurement -- so this doubles as its real, final
    // positioning. contentWidth itself is stable across every call (every
    // panel that defines it has a compile-time-constant width, unaffected by
    // mode/style), so windowWidth never actually changes here -- only
    // windowHeight does.
    layoutUniversalControlsRow (0); // side effect only here -- layoutControlsContent() below re-derives the actual height it needs

    const int contentWidth = juce::jmax (trimTempoSectionPanel.getRight(), pitchModeSectionPanel.getRight(), minContentWidth);

    const int controlsTotalHeight = layoutControlsContent (contentWidth); // Layers 1-4, real final positioning

    // Layer 5's own real positioning, at whatever width subModeViewport's
    // vertical scrollbar leaves it (same "reduce by scrollbar thickness"
    // convention sequencerViewport/sequencerGrid already use).
    const int subModeContentWidth = contentWidth - subModeViewport.getScrollBarThickness();
    const int subModeHeight = layoutSubModeContent (subModeContentWidth);
    subModeContent.setSize (subModeContentWidth, subModeHeight);

    const int windowWidth = contentWidth + windowMargin * 2;
    const int windowHeight = windowMargin * 2 + headerTextHeight + controlsTotalHeight + layoutGap
                            + subModeViewportHeight
                            + controlsToZoomGap + zoomRowHeight + zoomToWaveformGap + minWaveformHeight;

    setSize (windowWidth, windowHeight);
}

int SlicerAudioProcessorEditor::layoutTopToolbar (int startY)
{
    // Layer 1 (Pass 4) -- Sample (Load button, status label) only.
    // Everything else in the universal layer (Reset Edits+Undo+Redo, Tempo,
    // Detection, Fade In/Out, Pitch Mode) is laid out by
    // layoutUniversalControlsRow() below.
    constexpr int sectionSacrificialHeight = 400;
    constexpr int sampleWidth = 220;

    sampleSectionPanel.setBounds (0, startY, sampleWidth, sectionSacrificialHeight);
    auto content = sectionContentArea (sampleSectionPanel);
    const int startHeight = content.getHeight();

    auto topRow = content.removeFromTop (36);
    loadButton.setBounds (topRow.removeFromLeft (loadButton.getBestWidthForHeight (36) + 24));
    content.removeFromTop (10);
    statusLabel.setBounds (content.removeFromTop (30));

    const int consumed = startHeight - content.getHeight();
    const int panelHeight = SectionPanel::titleBarHeight + consumed + 12; // 12 = getContentArea()'s reduced(8,6) top+bottom padding
    sampleSectionPanel.setBounds (0, startY, sampleWidth, panelHeight);

    return panelHeight;
}

int SlicerAudioProcessorEditor::layoutUniversalControlsRow (int startY)
{
    // Universal controls row (Pass 4/6) -- Reset Edits+Undo+Redo, Tempo,
    // Detection, Fade In/Out, Pitch Mode, arranged as two explicit rows of
    // content-sized panels/clusters rather than a generic wrap-at-width
    // algorithm -- same approach layoutTopToolbar() itself used pre-Pass-4,
    // before Sample split out into its own Layer 1: each panel's own
    // natural width is what DEFINES contentWidth for every layer below it
    // (see the constructor's one-time measurement pass), not the other way
    // around, so there's no contentWidth to wrap against here in the first
    // place.
    constexpr int panelGap = 14;
    constexpr int sectionSacrificialHeight = 400;

    // Same measure-then-shrink two-pass SectionPanel sizing the old
    // layoutGlobalSection() used, just parameterised over an explicit
    // (x, y, width) instead of a shared left-to-right area, since panels
    // here sit side by side in two rows rather than stacked in one column.
    auto layoutPanel = [] (SectionPanel& panel, int x, int y, int width, auto&& layoutFn) -> int
    {
        panel.setBounds (x, y, width, sectionSacrificialHeight);
        auto content = sectionContentArea (panel);
        const int startHeight = content.getHeight();

        layoutFn (content);

        const int consumed = startHeight - content.getHeight();
        const int panelHeight = SectionPanel::titleBarHeight + consumed + 12; // 12 = getContentArea()'s reduced(8,6) top+bottom padding
        panel.setBounds (x, y, width, panelHeight);
        return panelHeight;
    };

    int row1X = 0;
    const int row1Y = startY;

    // Tempo -- Audition, Loop Length number box + BPM label, Manual BPM override.
    constexpr int tempoWidth = 340;
    const int tempoHeight = layoutPanel (trimTempoSectionPanel, row1X, row1Y, tempoWidth, [this] (juce::Rectangle<int>& content)
    {
        auto auditionRow = content.removeFromTop (30);
        auditionButton.setBounds (auditionRow.removeFromLeft (auditionButton.getBestWidthForHeight (30) + 24));
        content.removeFromTop (10);

        auto loopRow = content.removeFromTop (30);
        loopLengthLabel.setBounds (loopRow.removeFromLeft (110).withSizeKeepingCentre (110, 20));
        loopLengthSlider.setBounds (loopRow.removeFromLeft (100));
        loopRow.removeFromLeft (10);
        calculatedBpmLabel.setBounds (loopRow);
        content.removeFromTop (14);

        manualBpmOverrideToggle.setBounds (content.removeFromTop (24));
        content.removeFromTop (6);

        auto manualBpmValueRow = content.removeFromTop (30);
        manualBpmOverrideLabel.setBounds (manualBpmValueRow.removeFromLeft (60));
        manualBpmOverrideSlider.setBounds (manualBpmValueRow);
    });
    row1X += tempoWidth + panelGap;

    // Reset Edits + Undo + Redo -- grouped together, bare buttons with no
    // SectionPanel chrome, same convention Zoom controls already use below
    // the waveform. Vertically centred against Tempo's own height -- the
    // tallest thing in this row now that Sample no longer sits alongside it.
    const int resetWidth = resetEditsButton.getBestWidthForHeight (30) + 24;
    constexpr int undoWidth = 100, redoWidth = 100, buttonGap = 10;
    const int resetClusterWidth = resetWidth + buttonGap + undoWidth + buttonGap + redoWidth;
    {
        auto row = juce::Rectangle<int> (row1X, row1Y + (tempoHeight - 30) / 2, resetClusterWidth, 30);
        resetEditsButton.setBounds (row.removeFromLeft (resetWidth));
        row.removeFromLeft (buttonGap);
        undoButton.setBounds (row.removeFromLeft (undoWidth));
        row.removeFromLeft (buttonGap);
        redoButton.setBounds (row.removeFromLeft (redoWidth));
    }
    row1X += resetClusterWidth + panelGap;

    const int row1Height = juce::jmax (tempoHeight, 30);

    int row2X = 0;
    const int row2Y = row1Y + row1Height + panelGap;

    // Detection -- Transient sensitivity number box, Quantize Transients + Grid.
    constexpr int detectionWidth = 300;
    const int detectionHeight = layoutPanel (detectionSectionPanel, row2X, row2Y, detectionWidth, [this] (juce::Rectangle<int>& content)
    {
        auto sensitivityRow = content.removeFromTop (30);
        sensitivityLabel.setBounds (sensitivityRow.removeFromLeft (110).withSizeKeepingCentre (110, 20));
        sensitivitySlider.setBounds (sensitivityRow.removeFromLeft (100));
        content.removeFromTop (14);

        quantizeTransientsToggle.setBounds (content.removeFromTop (24));
        content.removeFromTop (6);

        auto quantizeGridRow = content.removeFromTop (30);
        quantizeGridLabel.setBounds (quantizeGridRow.removeFromLeft (60));
        quantizeGridSelector.setBounds (quantizeGridRow.removeFromLeft (150));
    });
    row2X += detectionWidth + panelGap;

    // Fade In/Out -- its own panel, split out of Tempo to match the spec's
    // separate item.
    constexpr int fadeWidth = 260;
    const int fadeHeight = layoutPanel (fadeSectionPanel, row2X, row2Y, fadeWidth, [this] (juce::Rectangle<int>& content)
    {
        auto fadeInRow = content.removeFromTop (30);
        fadeInLabel.setBounds (fadeInRow.removeFromLeft (80));
        fadeInSlider.setBounds (fadeInRow);
        content.removeFromTop (10);

        auto fadeOutRow = content.removeFromTop (30);
        fadeOutLabel.setBounds (fadeOutRow.removeFromLeft (80));
        fadeOutSlider.setBounds (fadeOutRow);
    });
    row2X += fadeWidth + panelGap;

    // Pitch Mode -- mode-aware height (Pass 5): only the ACTIVE sub-section's
    // own controls are laid out/reserved now -- Repitch's single toggle, OR
    // Time-Stretch's own pitchModeExtraViewport (small fixed height, keeping
    // the nested-viewport fix exactly as already built) -- instead of both
    // unconditionally regardless of which mode is selected. The editor
    // resizes itself (updateWindowSize()) whenever pitchModeSegments
    // changes, since this panel's own height now varies by mode.
    constexpr int pitchModeWidth = 320;
    constexpr int pitchModeExtraViewportHeight = 90;
    const bool timeStretchActive = pitchModeSegments.getSelectedIndex() == 1;
    const int pitchModeHeight = layoutPanel (pitchModeSectionPanel, row2X, row2Y, pitchModeWidth, [this, timeStretchActive] (juce::Rectangle<int>& content)
    {
        pitchModeLabel.setBounds (content.removeFromTop (20));
        pitchModeSegments.setBounds (content.removeFromTop (SegmentedButtonRow::preferredHeight));
        content.removeFromTop (10);

        if (timeStretchActive)
        {
            pitchModeExtraViewport.setBounds (content.removeFromTop (pitchModeExtraViewportHeight));
        }
        else
        {
            beatQuantizeToggleRepitch.setBounds (content.removeFromTop (24));

            // Not part of Repitch's own flow (zero height, so it consumes no
            // space here -- and it's already setVisible(false) via
            // updatePitchModeVisibility(), so it neither paints nor hit-tests
            // regardless), but still needs a real WIDTH matching this panel's
            // actual content width, so the pitchModeExtraContent sizing block
            // just below always has real numbers -- not stale/zero bounds
            // left over from whenever Time-Stretch was last active (or never,
            // at first startup with Repitch as the default mode).
            pitchModeExtraViewport.setBounds (content.getX(), content.getY(), content.getWidth(), 0);
        }
    });

    // pitchModeExtraContent's own internal row layout -- local coordinates
    // starting at (0, 0), same measure-then-shrink idea as everything else
    // in this function, just for a Viewport's content instead of a
    // SectionPanel.
    {
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

    const int row2Height = juce::jmax (detectionHeight, fadeHeight, pitchModeHeight);

    return row1Height + panelGap + row2Height;
}

int SlicerAudioProcessorEditor::layoutPlaybackStyleSection (int contentWidth, int startY)
{
    // Playback Style, directly below the universal controls row (Pass 4/5/6).
    // Laid out unconditionally by layoutControlsContent() (see its own doc
    // comment), but WHAT it lays out -- and so the height it reports --
    // depends on the active sub-mode tab: Generate gets the full selector +
    // probability table + parameter panel (bound to the global default);
    // Sequence gets the probability table alone; Control/Perform get none of
    // it (Perform has its own separate performanceStyleParameterPanel
    // elsewhere instead, bound to the focused state's own working
    // accessors). Actual show/hide is applied by
    // updateActiveTabVisibility(), not here -- this only decides bounds.
    constexpr int sectionSacrificialHeight = 1200;

    const int subIndex = subModeTabs.getSelectedIndex();
    const bool showTable = subIndex == 0 || subIndex == 1;   // Generate or Sequence
    const bool showSelectorAndParams = subIndex == 0;        // Generate only

    if (! showTable)
    {
        playbackStyleSectionPanel.setBounds (0, startY, contentWidth, 0);
        return 0;
    }

    // Parameter viewport height (Pass 5) -- capped at 80 (same "comfortably
    // shows a handful of rows; scrolls for the rest" budget Pass 3
    // introduced), but sized to the CURRENTLY SELECTED style's own actual
    // row count instead of always paying that flat 80 regardless of which
    // style is active. The editor resizes itself (updateWindowSize())
    // whenever playbackStyleSegments changes, since this height now varies
    // by style.
    constexpr int maxParameterViewportHeight = 80;
    const int selectedStyle = juce::jlimit (0, SlicerAudioProcessor::numPlaybackStyleOptions - 1, playbackStyleSegments.getSelectedIndex());
    const int selectedStylePreferredHeight = PlaybackStyleParameterPanel::getPreferredHeightForStyle (selectedStyle);
    const int parameterViewportHeight = juce::jmin (maxParameterViewportHeight, selectedStylePreferredHeight);

    playbackStyleSectionPanel.setBounds (0, startY, contentWidth, sectionSacrificialHeight);
    auto content = sectionContentArea (playbackStyleSectionPanel);
    const int startHeight = content.getHeight();

    if (showSelectorAndParams)
    {
        playbackStyleSegments.setBounds (content.removeFromTop (SegmentedButtonRow::preferredHeight));
        content.removeFromTop (10);
    }

    playbackStyleLabel.setBounds (content.removeFromTop (20));
    playbackStyleGrid.setBounds (content.removeFromTop (PlaybackStyleGrid::getPreferredHeight()));

    if (showSelectorAndParams)
    {
        content.removeFromTop (14);
        playbackStyleParametersLabel.setBounds (content.removeFromTop (20));
        playbackStyleParameterViewport.setBounds (content.removeFromTop (parameterViewportHeight));
        playbackStyleParameterPanel.setSize (playbackStyleParameterViewport.getWidth() - playbackStyleParameterViewport.getScrollBarThickness(),
                                              selectedStylePreferredHeight);
    }

    const int consumed = startHeight - content.getHeight();
    const int panelHeight = SectionPanel::titleBarHeight + consumed + 12;
    playbackStyleSectionPanel.setBounds (0, startY, contentWidth, panelHeight);

    return panelHeight;
}

int SlicerAudioProcessorEditor::layoutGenerateTab (int contentWidth, int startY)
{
    // Generate's own local content (Pass 2) -- everything else that used to
    // live here (Sample/Trim & Tempo/Detection/Pitch Mode) is part of the
    // universal layer now (Pass 6), laid out by layoutTopToolbar()/
    // layoutUniversalControlsRow() above this on every tab; Playback Style
    // (layoutPlaybackStyleSection()) is also laid out above this, mode-
    // specific but fully shown for Generate specifically. All that's left
    // here is which of Slice Length/Clock times the engine, plus whichever
    // of that choice's own sub-controls apply.
    constexpr int sacrificialHeight = 4000;
    constexpr int panelGap = 14;
    constexpr int sectionSacrificialHeight = 1200;

    juce::Rectangle<int> area (0, startY, contentWidth, sacrificialHeight);

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

        auto resetEveryRow = content.removeFromTop (30);
        resetEveryLabel.setBounds (resetEveryRow.removeFromLeft (140));
        resetEverySelector.setBounds (resetEveryRow.removeFromLeft (150));
        content.removeFromTop (10);

        subdivisionTableLabel.setBounds (content.removeFromTop (20));
        subdivisionGrid.setBounds (content.removeFromTop (SubdivisionProbabilityGrid::getPreferredHeight()));
    });

    return sacrificialHeight - area.getHeight(); // total consumed height, independent of startY (only position, not the height field, is offset by it)
}

int SlicerAudioProcessorEditor::layoutSequenceTab (int contentWidth, int startY)
{
    constexpr int sacrificialHeight = 4000;
    juce::Rectangle<int> area (0, startY, contentWidth, sacrificialHeight);

    auto stepResolutionRow = area.removeFromTop (30);
    stepResolutionLabel.setBounds (stepResolutionRow.removeFromLeft (140));
    stepResolutionSelector.setBounds (stepResolutionRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto patternLengthRow = area.removeFromTop (30);
    patternLengthLabel.setBounds (patternLengthRow.removeFromLeft (140));
    patternLengthSelector.setBounds (patternLengthRow.removeFromLeft (150));
    area.removeFromTop (10);

    // Sized to actual content (Pass 2) rather than stretched to fill the
    // row, same reasoning as Load Sample in layoutTopToolbar()/Audition in
    // layoutUniversalControlsRow().
    auto randomizeClearRow = area.removeFromTop (30);
    randomizeSequenceButton.setBounds (randomizeClearRow.removeFromLeft (randomizeSequenceButton.getBestWidthForHeight (30) + 24));
    randomizeClearRow.removeFromLeft (10);
    clearSequenceButton.setBounds (randomizeClearRow.removeFromLeft (clearSequenceButton.getBestWidthForHeight (30) + 24));
    area.removeFromTop (10);

    // This row's own height must accommodate whichever of the two is
    // taller: sequencerViewportHeight (the viewport's own fixed, scrolling
    // height -- unrelated to style count) or playbackStylePalette's
    // current preferred height (one row per PlaybackStyle, via
    // PlaybackStylePalette::getPreferredHeight() -- grows automatically as
    // styles are added, e.g. Scratch's addition already grew it past the
    // old fixed sequencerViewportHeight, which used to just clip/squash
    // the palette's own bottom row(s) short since setBounds() overrides
    // whatever height the palette's constructor gave itself). Not
    // hardcoded either way, so this needs no revisiting next time another
    // style is added.
    const int sequencerRowHeight = juce::jmax (sequencerViewportHeight, PlaybackStylePalette::getPreferredHeight());
    auto sequencerRow = area.removeFromTop (sequencerRowHeight);
    playbackStylePalette.setBounds (sequencerRow.removeFromLeft (PlaybackStylePalette::preferredWidth)
                                                 .removeFromTop (PlaybackStylePalette::getPreferredHeight()));
    sequencerRow.removeFromLeft (10);
    sequencerViewport.setBounds (sequencerRow);
    // Dynamic row-height scaling (see SequencerGrid's own doc comment) --
    // sequencerViewport's own height IS the available vertical space the
    // grid scales its rows against, so it's passed straight through.
    sequencerGrid.setAvailableHeight (sequencerViewport.getHeight());
    area.removeFromTop (20);

    // Pattern bank (MIDI input) -- its own row, rather than squeezed into
    // sequencerRow where it'd fight sequencerViewport for width.
    auto patternBankRow = area.removeFromTop (PatternBankPanel::getPreferredHeight());
    patternBankPanel.setBounds (patternBankRow.removeFromLeft (PatternBankPanel::preferredWidth));
    area.removeFromTop (20);

    auto patternSwitchTimingRow = area.removeFromTop (30);
    patternSwitchTimingLabel.setBounds (patternSwitchTimingRow.removeFromLeft (140));
    patternSwitchTimingSelector.setBounds (patternSwitchTimingRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto patternSwitchIntervalRow = area.removeFromTop (30);
    patternSwitchIntervalLabel.setBounds (patternSwitchIntervalRow.removeFromLeft (140));
    patternSwitchIntervalSelector.setBounds (patternSwitchIntervalRow.removeFromLeft (150));
    area.removeFromTop (10);

    return sacrificialHeight - area.getHeight(); // total consumed height, independent of startY (only position, not the height field, is offset by it)
}

int SlicerAudioProcessorEditor::layoutPerformTab (int contentWidth, int startY)
{
    constexpr int sacrificialHeight = 4000;
    juce::Rectangle<int> area (0, startY, contentWidth, sacrificialHeight);

    performanceStyleParametersLabel.setBounds (area.removeFromTop (20));
    performanceStyleParameterPanel.setBounds (area.removeFromTop (PlaybackStyleParameterPanel::getPreferredHeight()));
    area.removeFromTop (20);

    auto performanceToggleRow = area.removeFromTop (24);
    performanceLoopToggle.setBounds (performanceToggleRow.removeFromLeft (100));
    performanceToggleRow.removeFromLeft (10);
    performanceSyncToggle.setBounds (performanceToggleRow.removeFromLeft (100));
    area.removeFromTop (10);

    // Trim Snap mode + its Grid-resolution picker.
    auto performanceTrimSnapRow = area.removeFromTop (30);
    performanceTrimSnapLabel.setBounds (performanceTrimSnapRow.removeFromLeft (140));
    performanceTrimSnapSelector.setBounds (performanceTrimSnapRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceTrimGridRow = area.removeFromTop (30);
    performanceTrimGridLabel.setBounds (performanceTrimGridRow.removeFromLeft (140));
    performanceTrimGridSelector.setBounds (performanceTrimGridRow.removeFromLeft (150));
    area.removeFromTop (10);

    // Quantize Recall toggle + its own note-value picker.
    auto performanceQuantizeRecallRow = area.removeFromTop (24);
    performanceQuantizeRecallToggle.setBounds (performanceQuantizeRecallRow.removeFromLeft (150));
    area.removeFromTop (10);

    auto performanceQuantizeRecallIntervalRow = area.removeFromTop (30);
    performanceQuantizeRecallIntervalLabel.setBounds (performanceQuantizeRecallIntervalRow.removeFromLeft (140));
    performanceQuantizeRecallIntervalSelector.setBounds (performanceQuantizeRecallIntervalRow.removeFromLeft (150));
    area.removeFromTop (10);

    // Full contentWidth, unlike the old compact bank grid -- a real
    // keyboard needs the room to be usable.
    auto performanceKeyboardRow = area.removeFromTop (PerformanceKeyboardPanel::getPreferredHeight());
    performanceKeyboardPanel.setBounds (performanceKeyboardRow);

    return sacrificialHeight - area.getHeight(); // total consumed height, independent of startY (only position, not the height field, is offset by it)
}

int SlicerAudioProcessorEditor::layoutControlTab (int contentWidth, int startY)
{
    constexpr int sacrificialHeight = 4000;
    juce::Rectangle<int> area (0, startY, contentWidth, sacrificialHeight);

    auto baseNoteRow = area.removeFromTop (30);
    controlBaseNoteLabel.setBounds (baseNoteRow.removeFromLeft (90));
    controlBaseNoteSlider.setBounds (baseNoteRow.removeFromLeft (140));
    baseNoteRow.removeFromLeft (10);
    controlBaseNoteNameLabel.setBounds (baseNoteRow.removeFromLeft (60));
    area.removeFromTop (6);

    controlSliceRangeLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (14);

    auto gateRow = area.removeFromTop (30);
    controlGateModeLabel.setBounds (gateRow.removeFromLeft (90));
    controlGateModeSegments.setBounds (gateRow.removeFromLeft (160));
    area.removeFromTop (18);

    controlKeyswitchSectionLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);

    for (auto& label : controlKeyswitchLabels)
    {
        label.setBounds (area.removeFromTop (22));
        area.removeFromTop (4);
    }

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

    // Polled rather than driven only by the button's own click, since the
    // processor can also stop an audition on its own (host transport
    // started) — the label/colour has to reflect that auto-stop too.
    const bool auditioning = processor.getAuditionActive();
    auditionButton.setButtonText (auditioning ? "Stop Audition" : "Audition");
    auditionButton.setColour (juce::TextButton::buttonColourId,
                               auditioning ? juce::Colours::orange.withAlpha (0.6f)
                                           : getLookAndFeel().findColour (juce::TextButton::buttonColourId));

    // These two are otherwise write-only (UI -> processor via onChange
    // below) -- a MIDI pattern-bank recall can change the processor's
    // stepResolutionIndex/patternLengthBarsIndex on its own, out from under
    // them, so poll and resync rather than let them show a stale value
    // while SequencerGrid (which already polls dimensions live) shows the
    // newly recalled grid underneath.
    const int processorStepResolutionId = processor.getStepResolutionIndex() + 1;
    if (stepResolutionSelector.getSelectedId() != processorStepResolutionId)
        stepResolutionSelector.setSelectedId (processorStepResolutionId, juce::dontSendNotification);

    const int processorPatternLengthId = processor.getPatternLengthBarsIndex() + 1;
    if (patternLengthSelector.getSelectedId() != processorPatternLengthId)
        patternLengthSelector.setSelectedId (processorPatternLengthId, juce::dontSendNotification);

    // Performance mode's working state -- same "poll and resync" reasoning
    // as the pair just above: a click on the on-screen keyboard
    // (setFocusedPerformanceStateSlot()) loads a new slot straight into
    // performanceWorkingState from the keyboard panel's own click handler,
    // out from under this parameter panel/these toggles, so a focus change
    // made from the keyboard has to be reflected here too, not just heard.
    // Also unconditionally repaints the parameter panel -- its rows read
    // performanceWorkingState live via
    // the getValue lambda passed to its constructor, but nothing else
    // triggers a repaint when that value changes from outside a drag on
    // this panel itself.
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
    // Generate-page-only (Pass 2) -- Sequenced/Performance's own controls
    // live on the Sequence/Perform tabs, gated by updateActiveTabVisibility()
    // instead, and Generate's own timing section (Slice Length/Clock) only
    // exists on Generate itself. Only called while Generate is actually the
    // active tab (from updateActiveTabVisibility() itself, or from
    // sliceLengthClockSegments' own onSelectionChanged, which can only fire
    // while that control is visible) -- no need to re-check that here.
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

    // Reset every (Step 34) — Slice Length mode only, since Clock mode
    // already has its own window-boundary mechanism.
    resetEveryLabel.setVisible (sliceLength);
    resetEverySelector.setVisible (sliceLength);
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
    // Grid resolution only matters -- and is only shown -- while Performance
    // mode is the active trigger mode AND Grid is the selected Trim Snap
    // mode; Transients needs no resolution picker at all. Reads
    // processor.getTriggerMode() directly now (Pass 1) rather than a UI
    // selector's index -- more robust than re-deriving "is Performance
    // active" from tab state, and this control only exists (and this
    // function is only called) while the Perform tab is actually showing it.
    const bool performance = processor.getTriggerMode() == SlicerAudioProcessor::TriggerMode::performance;
    const bool grid = performanceTrimSnapSelector.getSelectedId() == 2;

    performanceTrimGridLabel.setVisible (performance && grid);
    performanceTrimGridSelector.setVisible (performance && grid);
}

void SlicerAudioProcessorEditor::updatePerformanceQuantizeRecallVisibility()
{
    // Recall interval only matters -- and is only shown -- while Performance
    // mode is the active trigger mode AND Quantize Recall is switched on;
    // off needs no interval picker at all (immediate, unchanged behaviour).
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
        return;
    }

    const int topNote = juce::jmin (127, baseNote + numSlices - 1);
    controlSliceRangeLabel.setText (
        "Slices: " + juce::MidiMessage::getMidiNoteName (baseNote, true, true, 3) + " - "
            + juce::MidiMessage::getMidiNoteName (topNote, true, true, 3)
            + " (" + juce::String (numSlices) + ")",
        juce::dontSendNotification);
}

void SlicerAudioProcessorEditor::updateControlKeyswitchRows()
{
    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
    {
        const int note = processor.getControlKeyswitchNote (i);
        const juce::String noteName = (note >= 0 && note < 128)
            ? juce::MidiMessage::getMidiNoteName (note, true, true, 3)
            : juce::String ("unreachable"); // base note set low enough that this style's fixed slot falls outside 0-127

        controlKeyswitchLabels[(size_t) i].setText (
            SlicerAudioProcessor::getPlaybackStyleName (i) + ": " + noteName, juce::dontSendNotification);
    }
}

void SlicerAudioProcessorEditor::updatePitchModeVisibility()
{
    const int selectedIndex = pitchModeSegments.getSelectedIndex();
    const bool repitch = selectedIndex == 0;
    const bool timeStretch = selectedIndex == 1;

    beatQuantizeToggleRepitch.setVisible (repitch);

    // pitchModeExtraViewport itself (Pass 5), not just its inner content --
    // layoutBeatsControlsRow() now only lays this out (gives it real bounds)
    // while Time-Stretch is active, but that alone doesn't hide it if it's
    // still carrying stale nonzero bounds from the last time Time-Stretch
    // WAS active (bounds outlive a mode switch away from Time-Stretch, since
    // Repitch's own layout pass never touches this component at all). An
    // explicit setVisible(false) makes those stale bounds harmless -- no
    // paint, no mouse hit-testing, no visible empty scrollbar sitting in
    // Repitch's own smaller panel.
    pitchModeExtraViewport.setVisible (timeStretch);
    grainSizeLabel.setVisible (timeStretch);
    grainSizeSlider.setVisible (timeStretch);
    windowShapeLabel.setVisible (timeStretch);
    windowShapeSelector.setVisible (timeStretch);
    beatQuantizeToggle.setVisible (timeStretch);
    pitchShiftLabel.setVisible (timeStretch);
    pitchShiftSlider.setVisible (timeStretch);
}

void SlicerAudioProcessorEditor::updateActiveTabVisibility()
{
    const int subIndex = subModeTabs.getSelectedIndex();
    const bool generateActive = subIndex == 0;
    const bool sequenceActive = subIndex == 1;
    const bool controlActive = subIndex == 2;
    const bool performActive = subIndex == 3;

    // Single universal layer (Pass 6) -- Sample plus what used to be the
    // separate Beats-specific block (Reset Edits+Undo+Redo, Tempo,
    // Detection, Fade In/Out, Pitch Mode), unconditionally visible
    // regardless of which sub-mode tab is active.
    for (auto* c : universalComponents)
        c->setVisible (true);

    // Fine-grained sub-visibility WITHIN the universal layer (Repitch vs
    // Time-Stretch, Grid dropdown, BPM field).
    updatePitchModeVisibility();
    updateManualBpmOverrideVisibility();
    updateQuantizeTransientsVisibility();

    // Playback Style area -- mode-specific (Pass 6): Generate shows the
    // probability table + style-selector + parameter panel, bound to the
    // global default; Sequence shows the probability table alone (Randomize
    // reads from it); Control/Perform show none of it -- Perform has its own
    // separate performanceStyleParameterPanel below instead (see
    // performComponents), bound to the focused state's own working
    // accessors, untouched by this group.
    const bool showPlaybackStyleTable = generateActive || sequenceActive;
    const bool showPlaybackStyleSelectorAndParams = generateActive;
    playbackStyleSectionPanel.setVisible (showPlaybackStyleTable);
    playbackStyleLabel.setVisible (showPlaybackStyleTable);
    playbackStyleGrid.setVisible (showPlaybackStyleTable);
    playbackStyleSegments.setVisible (showPlaybackStyleSelectorAndParams);
    playbackStyleParametersLabel.setVisible (showPlaybackStyleSelectorAndParams);
    playbackStyleParameterViewport.setVisible (showPlaybackStyleSelectorAndParams);

    for (auto* c : generateComponents)
        c->setVisible (generateActive);

    if (generateActive)
    {
        // Generate's own local timing section (Slice Length/Clock and
        // whichever of its own sub-controls apply).
        updateSliceLengthClockVisibility();
    }

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
        updateControlKeyswitchRows();
    }
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
    // deliberately raw-stores trim (see setFocusedPerformanceStateSlot()'s
    // own doc comment) rather than rebuilding slices, so `slices` can be
    // stale against wherever Performance mode left the trim. Force one
    // rebuild here -- a one-off UI-thread action, not a hot path -- by
    // re-invoking the existing public trim setter with the same value it
    // already has, purely for its rebuild side effect (same fix the old
    // old triggerModeSelector.onChange applied).
    if (wasPerformance)
        processor.setTrimEndSample (processor.getTrimEndSample(), false);

    // Only refresh Generate's own Clock-only/Slice-Length-only sub-groups if
    // Generate is actually the active tab right now -- updateSliceLengthClockVisibility()
    // has no active-tab guard of its own (it trusts callers, same convention
    // this editor always used), so calling it while e.g. the Sequence tab is
    // active would incorrectly re-show Generate-only controls that
    // updateActiveTabVisibility()'s blanket hide (called just before this,
    // from both tab rows' onSelectionChanged) already turned off.
    if (subModeTabs.getSelectedIndex() == 0)
        updateSliceLengthClockVisibility();
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

    updateControlBaseNoteDisplay(); // Control mode's slice-range readout depends on the current slice count
    waveformDisplay.refresh();
}
