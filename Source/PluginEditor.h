#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "SubdivisionProbabilityGrid.h"
#include "PlaybackStyleFaderRow.h"
#include "SequencerGrid.h"
#include "PlaybackStyleSwatchRow.h"
#include "PlaybackStyleParameterPanel.h"
#include "PatternBankPanel.h"
#include "PerformanceKeyboardPanel.h"
#include "ControlKeyboardPanel.h"
#include "NeditPalette.h"
#include "SegmentedButtonRow.h"
#include "SectionPanel.h"
#include "PanelBackdrop.h"
#include "DragNumberField.h"
#include "AdjustmentsIconButton.h"

//==============================================================================
/** UI Pass 7 -- compact redesign. The old "single universal layer" (Sample,
    Reset Edits+Undo+Redo, Tempo, Detection, Fade In/Out, Pitch Mode -- five
    stacked SectionPanels, always visible above every sub-mode tab) is gone.
    In its place:

      - The waveform panel (layoutWaveformPanel()) -- Load Sample, Audition,
        Loop Length (+ live BPM readout), Fade In/Out, a Quantize Transients
        checkbox, Zoom to Trims/Reset Zoom, and Pitch Mode all folded into
        one slim, muted toolbar row sitting directly above waveformDisplay,
        both wrapped in a single dark PanelBackdrop card -- "context for the
        waveform, not the main focus". Transient sensitivity, the Quantize
        Grid dropdown, Manual BPM override, Reset Edits/Undo/Redo, and every
        Time-Stretch-only control (grain size/window shape/beat-quantize/
        pitch shift) moved off this row entirely -- they're real but
        secondary, so they live behind advancedSettingsButton's popover
        instead (showWaveformAdvancedPopover(), a juce::CallOutBox built
        fresh each time it's clicked from the same long-lived member
        controls, reparented in and back out -- see that method's own doc
        comment for why this is safe).

      - Playback Style (selector, fader row, parameter panel) moved OUT of
        the old always-visible layer and into Generate's own tab content
        (layoutGenerateTab()), directly above Generate's existing local
        Timing section (Slice Length vs Clock -- unchanged). The old
        horizontal probability-slider list (PlaybackStyleGrid) is gone,
        replaced by PlaybackStyleFaderRow -- a row of compact vertical
        faders, one per style, coloured via
        PlaybackStylePalette::getStyleColour(). Sequence no longer shows a
        copy of this table at all (Pass 6 used to show it there too, purely
        for Randomize to read from) -- editing probabilities now happens
        exclusively on Generate, and Sequence's Randomize reads the exact
        same processor-owned values regardless of which tab last touched
        them.

      - Sequence's style palette shrank from a tall vertical sidebar
        (PlaybackStylePalette, one full row + checkbox per style) to
        PlaybackStyleSwatchRow -- small square swatches in a single
        horizontal row, sharing one slim row with Randomize/Clear and a
        compact pattern length/step resolution readout, so sequencerGrid
        itself gets the large majority of the tab's space (see
        layoutSequenceTab()).

      - Control's plain text keyswitch list (controlKeyswitchLabels, one
        Label per style) is gone, replaced by ControlKeyboardPanel -- a real
        piano keyboard (reusing juce::MidiKeyboardComponent, same pattern as
        PerformanceKeyboardPanel below) with a coloured wash across the
        keyswitch range and a separate wash across the slice-trigger range.
        Clicking a keyswitch key still reveals that style's parameters below
        (controlStyleParameterPanel), exactly as before -- just routed
        through ControlKeyboardPanel::Source::selectKeyswitchStyle() instead
        of a label's mouseDown.

      - Perform's on-screen keyboard (PerformanceKeyboardPanel) already drew
        a real black/white piano keyboard with focus/populated-slot
        highlights directly on the keys (see its own class doc comment) --
        confirmed close enough to the target look that it needed no rebuild
        for this pass, just more room (Perform's tab content, like every
        other tab now, gets real space instead of a small fixed viewport).

    subModeViewport/subModeContent (Layer 5) keep the exact Viewport-wrapping-
    a-Component shape from Pass 3, but subModeViewportHeight is no longer a
    small fixed constant -- updateWindowSize() now measures every tab's own
    real preferred height (layoutGenerateTab()/layoutSequenceTab()/
    layoutControlTab()/layoutPerformTab(), each already returns exactly that)
    and reserves the tallest one, so the active tab's full content is always
    visible without scrolling in practice; the Viewport itself stays purely
    as a safety net for an unexpectedly narrow/short host window, same
    "reserve worst case, never rely on scrolling" convention this codebase
    already used for pitchModeExtraViewport/playbackStyleParameterViewport. */
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
    void updateSliceLengthClockVisibility(); // shows/hides Generate's own Clock-only/Slice-Length-only controls
    void updatePitchModeVisibility(); // shows/hides the Time-Stretch-only controls (within the advanced popover)
    void updateManualBpmOverrideVisibility(); // shows/hides the BPM numeric field (within the advanced popover)
    void updateQuantizeTransientsVisibility(); // shows/hides the Grid dropdown (within the advanced popover)
    void updatePerformanceTrimSnapVisibility(); // shows/hides the Trim Snap Grid-resolution dropdown
    void updatePerformanceQuantizeRecallVisibility(); // shows/hides the Quantize Recall note-value dropdown
    void updateControlBaseNoteDisplay(); // refreshes the base-note name label and the slice-range readout
    void updateControlKeyboardPanel(); // refreshes ControlKeyboardPanel's visible range + selection highlight
    void updateControlStyleParameterPanelVisibility(); // shows/hides controlStyleParametersLabel/controlStyleParameterPanel based on controlSelectedKeyswitchStyle
    void onControlKeyswitchSelected (int styleIndex); // called by ControlKeyboardSource when a keyswitch key is clicked
    void onGenerateStyleSelected (int styleIndex); // called by playbackStyleFaderRow and playbackStyleSegments alike

    // Builds a fresh popover content Component every time advancedSettingsButton
    // is clicked, reparents the long-lived secondary controls (sensitivity,
    // quantize grid, manual BPM override, Reset/Undo/Redo, pitch-mode-extra
    // controls) into it, lays them out, and launches it via
    // juce::CallOutBox::launchAsynchronously(). Safe to reparent these:
    // they're stored as plain member objects (not owned by whatever they're
    // currently parented under), so when the CallOutBox closes and deletes
    // its (otherwise-empty) content Component, JUCE's Component destructor
    // only detaches children from their parent's list -- it never deletes
    // them -- leaving every control and its state fully intact, just
    // parentless again until the next click reparents them back in.
    void showWaveformAdvancedPopover();

    int layoutControlsContent (int contentWidth); // waveform panel + sub-mode tab row -- always fully visible, never scrolls; returns the total height it needs

    // Recomputes and re-applies the whole window's size. Called at
    // construction and again whenever Pitch Mode's selection changes (its
    // popover-only content still varies the popover's OWN size, not the
    // window's -- but is kept for parity/cheapness) or a sub-mode tab
    // switches (each tab's real preferred height can differ).
    void updateWindowSize();

    // Layer 5 -- dispatches to whichever of Generate/Sequence/Control/
    // Perform is actually active, laid out inside subModeContent (viewed
    // through subModeViewport). Returns the total height the active tab's
    // own content needs, which becomes subModeContent's size.
    int layoutSubModeContent (int contentWidth);

    // Lays out ONE tab's own controls, starting at local y == startY within
    // subModeContent. All other tabs' controls keep whatever bounds they
    // last had, but stay invisible via updateActiveTabVisibility() below, so
    // stale bounds are harmless. Each returns the total height its own
    // content needs (not including startY).
    int layoutGenerateTab (int contentWidth, int startY); // style selector/fader row/parameter panel, then Generate's own local Slice-Length-vs-Clock timing choice
    int layoutSequenceTab (int contentWidth, int startY); // slim swatches/Randomize/Clear/pattern-info row, then sequencerGrid at generous height, then the MIDI pattern bank
    int layoutPerformTab (int contentWidth, int startY); // existing Performance-mode flow, unchanged
    int layoutControlTab (int contentWidth, int startY); // base note, slice range readout, Trigger/Gate toggle, real piano keyboard + wash legend, revealed style parameters

    // The waveform panel -- Load/Audition/Loop Length+BPM/Fade/Quantize/
    // Zoom/Pitch Mode folded into one slim toolbar row, plus waveformDisplay
    // itself, both wrapped in waveformPanelBackdrop. Lives directly on
    // `this` (like the old zoom buttons did), NOT inside controlsContent --
    // see resized()'s own layout order. Returns the total height consumed.
    int layoutWaveformPanel (int startY, int width);

    // Absolute (controlsContent-local) content rect for a SectionPanel
    // that's already had setBounds() called -- title bar trimmed, standard
    // padding applied, same math as SectionPanel::getContentArea() but
    // translated into controlsContent's coordinate space (children of a
    // SectionPanel's content stay direct children of controlsContent, not
    // of the panel itself -- see SectionPanel's own class doc comment).
    static juce::Rectangle<int> sectionContentArea (const SectionPanel& panel);

    // Shows/hides every top-level page (Generate's sections, Sequence's
    // controls, Control's controls, Perform's controls), based on
    // subModeTabs' current selection.
    void updateActiveTabVisibility();

    // Maps the active sub-mode tab onto processor.setTriggerMode() --
    // Sequence tab -> sequenced, Control tab -> control, Perform tab ->
    // performance, Generate -> whichever of Slice Length/Clock Generate's
    // own local timing row last selected. Guards against calling
    // setTriggerMode() with the mode it's already in, since that method
    // unconditionally resets clock/reset/sequenced/performance init flags
    // and MIDI-learn state even for a same-mode call (see
    // SlicerAudioProcessor::setTriggerMode()'s own doc comment) -- every
    // tab-driven mode change must go through this, never call
    // processor.setTriggerMode() directly from a tab callback.
    void syncTriggerModeToActiveTab();

    SlicerAudioProcessor& processor;

    // Layer 1-2 (waveform panel + sub-mode tab row) live inside
    // controlsContent, a plain non-scrolling child -- always fully visible.
    // Layer 3 (the active tab) lives in subModeContent/subModeViewport
    // instead (see their own doc comment below).
    juce::Component controlsContent;

    // The active sub-mode tab's own content, viewed through subModeViewport
    // at a height reserved for the tallest tab (see updateWindowSize()'s own
    // doc comment) -- a safety-net Viewport, not a deliberately-small
    // scrolling region the way it used to be.
    juce::Viewport subModeViewport;
    juce::Component subModeContent;
    int subModeViewportHeight = 400; // recomputed every updateWindowSize() call

    // Applies the Tungsten/Salmon palette to every ComboBox/ToggleButton/
    // TextButton/Label/Slider still using its native JUCE widget type --
    // scoped to controlsContent/subModeContent/the advanced popover only, so
    // it never affects native dialog chrome (e.g. the file chooser).
    NeditPalette::LookAndFeel neditLookAndFeel;

    // Sub-mode tab row: Generate/Sequence/Control/Perform.
    SegmentedButtonRow subModeTabs;

    // Remembers which of Slice Length/Clock Generate's own Trigger Mode row
    // last selected -- processor.getTriggerMode() gets forced to sequenced/
    // performance while the Sequence/Perform tabs are active (see
    // syncTriggerModeToActiveTab()), so returning to the Generate tab needs
    // this to restore the right mode rather than always defaulting back to
    // Slice Length.
    SlicerAudioProcessor::TriggerMode lastGenerateTriggerMode = SlicerAudioProcessor::TriggerMode::sliceLength;

    //=== Waveform panel (compact) ===
    PanelBackdrop waveformPanelBackdrop;

    juce::TextButton loadButton { "Load" };
    juce::Label statusLabel;

    // Audition (Step 25) — plays [trimStart, trimEnd) on a tight raw loop,
    // independent of host transport. Label toggles between "Audition"/"Stop"
    // in timerCallback() since the processor can also stop it on its own
    // the instant host transport starts.
    juce::TextButton auditionButton { "Audition" };

    juce::Label loopLengthLabel;
    DragNumberField loopLengthField; // integer bars -- plain-text vertical-drag-to-scrub field, not a slider
    juce::Label calculatedBpmLabel;

    // Loop length staleness flag (Step 33) — set true whenever
    // waveformDisplay reports an actual trim change, cleared the moment the
    // user acknowledges by touching loopLengthField at all.
    bool loopLengthNeedsAttention = false;

    juce::Label fadeInLabel; // doubles as the compact row's single "Fade" caption -- fadeOutSlider shares it, no separate label
    juce::Slider fadeInSlider;
    juce::Slider fadeOutSlider;

    juce::ToggleButton quantizeTransientsToggle { "Quantize" };

    // Zoom/pan (Step 31) — small icon-glyph buttons, folded into the compact
    // toolbar row instead of their own row above the waveform.
    // Plain ASCII labels, not Unicode glyphs -- juce::TextButton's
    // const-char*-literal constructor assumes ASCII and asserts (a hard
    // trap in a debug build, with no attached debugger to catch it) on any
    // byte above 127, so a raw UTF-8 escape sequence here would need an
    // explicit juce::CharPointer_UTF8 wrap to be safe; short text is simpler.
    juce::TextButton zoomToTrimsButton { "Zoom" };
    juce::TextButton resetZoomButton { "Reset" };

    // No separate "Pitch mode" caption -- the segmented row's own two
    // segment labels ("Repitch"/"Time-Stretch") are self-explanatory.
    SegmentedButtonRow pitchModeSegments;

    // Advanced-settings popover trigger -- opens showWaveformAdvancedPopover().
    AdjustmentsIconButton advancedSettingsButton;

    //=== Advanced popover content (secondary waveform-panel controls) ===
    // Not permanently parented anywhere -- reparented into a fresh popover
    // Component each time advancedSettingsButton is clicked (see
    // showWaveformAdvancedPopover()'s own doc comment).
    juce::TextButton resetEditsButton { "Reset edits" };
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };

    juce::ToggleButton manualBpmOverrideToggle { "Manual BPM override" };
    juce::Label manualBpmOverrideLabel;
    juce::Slider manualBpmOverrideSlider;

    juce::Label sensitivityLabel;
    DragNumberField sensitivityField; // plain-text vertical-drag-to-scrub field, not a slider

    juce::Label quantizeGridLabel;
    juce::ComboBox quantizeGridSelector;

    juce::ToggleButton beatQuantizeToggleRepitch { "Beat-quantize slice length" };

    juce::Viewport pitchModeExtraViewport;
    juce::Component pitchModeExtraContent;
    juce::Label grainSizeLabel;
    juce::Slider grainSizeSlider;
    juce::Label windowShapeLabel;
    juce::ComboBox windowShapeSelector; // "Hann" / "Triangular"
    juce::ToggleButton beatQuantizeToggle { "Beat-quantize slice length" };
    juce::Label pitchShiftLabel;
    juce::Slider pitchShiftSlider; // semitones, -24 to +24

    WaveformDisplay waveformDisplay;

    //=== Generate tab ===
    // Playback style (selector, fader row, parameter panel) -- Generate-only
    // now, laid out by layoutGenerateTab() directly above Generate's own
    // Timing section.
    SegmentedButtonRow playbackStyleSegments; // small style-tab row above the fader row
    PlaybackStyleFaderRow playbackStyleFaderRow;

    juce::Label playbackStyleParametersLabel;
    PanelBackdrop generateParameterAreaBackdrop; // the mockup's dashed "remaining space" box
    PlaybackStyleParameterPanel playbackStyleParameterPanel;

    juce::Label sliceLengthClockLabel;
    SegmentedButtonRow sliceLengthClockSegments; // "Slice Length" / "Clock"
    SectionPanel timingSectionPanel { "Timing" };

    juce::Label clockReferenceLabel;
    juce::ComboBox clockReferenceSelector;
    juce::Label tapeStopScopeLabel;
    juce::ComboBox tapeStopScopeSelector;
    juce::Label filterSweepScopeLabel;
    juce::ComboBox filterSweepScopeSelector;
    juce::Label resetEveryLabel;
    juce::ComboBox resetEverySelector;
    juce::Label subdivisionTableLabel;
    SubdivisionProbabilityGrid subdivisionGrid;

    //=== Sequence tab ===
    // Compact corner readout (Pass 7) -- the selector's own selected text
    // ("1/16", "1 bar") already shows the value, so no separate label row
    // is needed for either, unlike every other ComboBox in this editor.
    juce::ComboBox stepResolutionSelector;
    juce::ComboBox patternLengthSelector;

    juce::TextButton randomizeSequenceButton { "Randomize" };
    juce::TextButton clearSequenceButton { "Clear" };

    PlaybackStyleSwatchRow playbackStyleSwatchRow;

    // Sequence's own probability fader row -- the original spec's "Sequence
    // shows the probability table only (Randomize reads from it)" (see
    // PlaybackStyleFaderRow's own class doc comment for Generate's copy of
    // the same idea). A separate PlaybackStyleFaderRow instance, not a
    // shared one with Generate's -- a juce::Component can only have one
    // parent at a time -- but both instances read/write the exact same
    // processor-owned per-style probabilities, so editing from either tab
    // is visible on the other. No onStyleSelected wiring: Sequence has no
    // "selected style's parameters" panel of its own (those stay in
    // SequencerGrid's per-step right-click popup), so a fader click here
    // only ever sets that style's probability, never a selection.
    PlaybackStyleFaderRow sequencePlaybackStyleFaderRow;

    struct SequencerBankSource : public PatternBankPanel::BankSource
    {
        explicit SequencerBankSource (SlicerAudioProcessor& p) : processor (p) {}
        void armSave() override { processor.armMidiLearnForPatternSave(); }
        void cancelLearn() override { processor.cancelMidiLearn(); }
        bool isLearnArmed() const override { return processor.isMidiLearnArmed(); }
        std::array<bool, 128> getPopulatedSlots() const override { return processor.getPopulatedPatternBankSlots(); }
        int getActiveSlot() const override { return processor.getActivePatternBankSlot(); }
        int getPendingSlot() const override { return processor.getPendingPatternSwitchSlot(); }
        SlicerAudioProcessor& processor;
    };

    SequencerBankSource sequencerBankSource;
    PatternBankPanel patternBankPanel;

    juce::Label patternSwitchTimingLabel;
    juce::ComboBox patternSwitchTimingSelector;
    juce::Label patternSwitchIntervalLabel;
    juce::ComboBox patternSwitchIntervalSelector;

    juce::Viewport sequencerViewport;
    SequencerGrid sequencerGrid;

    //=== Perform tab ===
    struct PerformanceKeyboardSource : public PerformanceKeyboardPanel::Source
    {
        explicit PerformanceKeyboardSource (SlicerAudioProcessor& p) : processor (p) {}
        void focusSlot (int noteNumber) override { processor.setFocusedPerformanceStateSlot (noteNumber); }
        std::array<bool, 128> getPopulatedSlots() const override { return processor.getPopulatedPerformanceStateBankSlots(); }
        int getFocusedSlot() const override { return processor.getFocusedPerformanceStateSlot(); }
        SlicerAudioProcessor& processor;
    };

    juce::Label performanceStyleParametersLabel;
    PlaybackStyleParameterPanel performanceStyleParameterPanel;
    juce::ToggleButton performanceLoopToggle { "Loop" };
    juce::ToggleButton performanceSyncToggle { "Sync" };

    juce::Label performanceTrimSnapLabel;
    juce::ComboBox performanceTrimSnapSelector;
    juce::Label performanceTrimGridLabel;
    juce::ComboBox performanceTrimGridSelector;

    juce::ToggleButton performanceQuantizeRecallToggle { "Quantize Recall" };
    juce::Label performanceQuantizeRecallIntervalLabel;
    juce::ComboBox performanceQuantizeRecallIntervalSelector;

    PerformanceKeyboardSource performanceKeyboardSource;
    PerformanceKeyboardPanel performanceKeyboardPanel;

    //=== Control tab ===
    juce::Label controlBaseNoteLabel;
    juce::Slider controlBaseNoteSlider;
    juce::Label controlBaseNoteNameLabel;
    juce::Label controlSliceRangeLabel;

    juce::Label controlGateModeLabel;
    SegmentedButtonRow controlGateModeSegments; // "Trigger" / "Gate"

    juce::Label controlLegendKeyswitchLabel; // "Keyswitches (styles)", tinted with ControlKeyboardPanel::keyswitchWashColour
    juce::Label controlLegendSliceLabel;     // "Slices", tinted with ControlKeyboardPanel::sliceWashColour

    // -1 until the user clicks a keyswitch key for the first time -- the
    // panel below stays hidden until then.
    int controlSelectedKeyswitchStyle = -1;
    juce::Label controlStyleParametersLabel;
    PlaybackStyleParameterPanel controlStyleParameterPanel;

    struct ControlKeyboardSource : public ControlKeyboardPanel::Source
    {
        explicit ControlKeyboardSource (SlicerAudioProcessorEditor& e) : editor (e) {}
        int getBaseNote() const override { return editor.processor.getControlBaseNote(); }
        int getNumSlices() const override { return editor.processor.getSequencerNumRows(); }
        int getSelectedKeyswitchStyle() const override { return editor.controlSelectedKeyswitchStyle; }
        void selectKeyswitchStyle (int styleIndex) override { editor.onControlKeyswitchSelected (styleIndex); }
        SlicerAudioProcessorEditor& editor;
    };

    ControlKeyboardSource controlKeyboardSource;
    ControlKeyboardPanel controlKeyboardPanel;

    // Every component that belongs to Generate/Sequence/Perform/Control
    // respectively -- populated once in the constructor and used purely for
    // blanket setVisible() in updateActiveTabVisibility().
    std::vector<juce::Component*> generateComponents;
    std::vector<juce::Component*> sequenceComponents;
    std::vector<juce::Component*> performComponents;
    std::vector<juce::Component*> controlComponents;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
