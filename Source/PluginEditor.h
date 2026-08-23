#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "SubdivisionProbabilityGrid.h"
#include "PlaybackStyleGrid.h"
#include "SequencerGrid.h"
#include "PlaybackStylePalette.h"
#include "PlaybackStyleParameterPanel.h"
#include "PatternBankPanel.h"
#include "PerformanceKeyboardPanel.h"
#include "NeditPalette.h"
#include "SegmentedButtonRow.h"
#include "SectionPanel.h"

//==============================================================================
/** Step-41 editor: load button, reset-edits safety net, undo/redo, an
    Audition button (plays the current trim on a tight raw loop,
    independent of host transport, auto-stopping the instant the
    transport starts, and available regardless of Pitch Mode), status
    label, loop-length/sensitivity controls (with a live preview while
    dragging sensitivity), a manual BPM override toggle + field that
    replaces the bars-derived tempo calculation entirely when enabled,
    fade controls, pitch mode (Repitch vs Time-Stretch — each with its
    own dedicated controls, shown/hidden as a group per mode: Repitch and
    Time-Stretch each get their own separate Beat-quantize-slice-length
    toggle, Time-Stretch's defaults ON since it's a free improvement there
    and Repitch's own separate toggle defaults OFF since quantizing there
    trades off pitch accuracy for beat-exact duration; Time-Stretch also
    has grain size/window shape/pitch shift controls), playback style
    (Forward / Ping-Pong / Tape Stop / Stretch / Filter
    Down / Filter Up / Bitcrush / Scratch / Flanger, rolled once per pick regardless of trigger mode —
    Stretch always renders through the granular engine regardless of pitch
    mode, with its own hardcoded small-grain/hard-edged-window character;
    Filter Down/Filter Up both apply a resonant low-pass swept log-scale
    across the pick's duration as post-processing on the output — Down
    ~9kHz down to ~250Hz, Up the mirror image — regardless of pitch mode,
    with a Filter Sweep scope selector (Whole Window vs Per Tick, Clock-
    mode-only, same visibility pattern as Tape Stop scope) controlling
    whether that sweep resets every tick or runs continuously across a
    whole Clock window — no other UI for either style), trigger
    mode (Slice Length vs Clock, with its clock-reference menu, Tape Stop
    scope selector, Filter Sweep scope selector, and subdivision
    probability grid — Slice Length mode instead gets a mandatory "Reset
    every" 1/2/4/8-bar selector, Step 34, forcing a hard resync at a fixed
    bar interval, since Slice Length's purely self-paced natural-completion
    scheduling has no other host-position awareness and can otherwise drift
    arbitrarily far from the beat grid) — laid out within a fixed,
    non-scrolling window sized once at construction to fit the tallest
    sub-mode tab's content in full (Pass 3 -- see controlsContent's own doc
    comment) — and, below a "Zoom to Trims"/"Reset Zoom" button pair
    (Step 31), the (now wider) waveform display, which stays outside
    controlsContent, always fully visible below it: it owns slice
    visualization, drag-and-
    drop loading, per-slice probability, manual slice add/move/remove,
    deleting auto-detected transients, a live generative playhead
    highlight, a dodger-blue Audition playhead line (Step 28, mutually
    exclusive with the generative one since the two engines can never run
    at once), modifier-key hover cues, a small beat-number grid (Step 31),
    and its own scroll-to-zoom/Shift-scroll-to-pan view range — every
    interaction above continues to work correctly at any zoom/pan state,
    not just fully zoomed out, since all of it maps through the same
    visible-range seam internally, and dragging a trim handle or manual
    point toward either edge of a zoomed-in view auto-pans to follow
    (Step 32), rather than stalling at the boundary. Trimming the sample
    doesn't auto-adjust Loop Length (bars) — since the tempo calculation
    it feeds depends on both — so an orange highlight (Step 33) appears
    around the Loop Length label/field whenever the trim actually changes,
    until the user touches that control at all (even re-entering the same
    value counts as acknowledgment). Right below Transient sensitivity, a
    "Quantize transients" toggle (Step 35) plus a Grid note-value dropdown
    (visible only while the toggle is on) snaps auto-detected transients —
    never manual points — onto the selected grid, correcting the noisy
    detection input at the root of most of this session's bugs. Trigger
    mode gained a third value, Sequenced (Step 37, v1, monophonic): a
    mouse-drawable step-sequencer grid (one row per available slice, one
    column per step at the chosen Step resolution) replaces the Clock-only
    controls, the mandatory Reset selector, and the Playback Style grid
    entirely in that mode -- structural monophony is enforced at the
    drawing level, and each active cell renders as a piano-roll bar
    spanning its slice's natural length, cut short by whatever the grid's
    own monophony will actually cut it off at during playback. Row 0
    renders at the BOTTOM of the grid (Step 38, standard piano-roll
    convention), the grid's width always matches WaveformDisplay's, a
    Pattern length (bars) dropdown (1/2/4, separate from Loop length,
    which only feeds the loaded audio's tempo calculation) sets the column
    count, and a Randomize Sequence button fills the pattern with
    constraint-aware random hits (each row's own slice length excludes
    where a next hit in that row may land). Every juce::Slider in this
    editor has scroll-wheel input disabled -- a holdover from when this
    editor scrolled internally; harmless now that it doesn't. A Clear Sequence button (Step 41) wipes the pattern
    with no generation, sitting next to Randomize; a Style Palette sidebar
    beside the grid lets the currently selected PlaybackStyle be chosen by
    colour swatch, and each sequencer cell now remembers and plays back
    its own style (Forward/Ping-Pong/Tape Stop/Stretch/Filter Down/Filter
    Up/Bitcrush/Scratch/Flanger) rather than always Forward -- Randomize draws each hit's style
    from the same weighted Playback Style probabilities Slice Length/Clock
    modes already use.

    Pass 6 -- Textures removed entirely (it never had a real engine, just a
    "coming soon" stub), collapsing what used to be a Beats-vs-Textures
    split back into one single universal layer: Sample (Load button, status
    label, laid out by layoutTopToolbar()) directly followed by Reset
    Edits+Undo+Redo, Tempo, Detection, Fade In/Out, Pitch Mode (laid out as a
    self-sizing two-row cluster by layoutUniversalControlsRow()) -- all of it
    unconditionally visible above all four Generate/Sequence/Control/Perform
    sub-mode tabs now, with no top-level tab row above them anymore. Playback
    Style (selector, probability weight table, parameter panel, laid out by
    layoutPlaybackStyleSection()) sits directly below that, but its own
    visibility is mode-specific rather than universal: Generate shows the
    probability table + style-selector + parameter panel, bound to the
    global default values (unchanged); Sequence shows the probability table
    only (Randomize reads from it), since per-step parameter editing there
    stays exclusively in SequencerGrid's own right-click popup; Perform shows
    neither -- it has its own separate performanceStyleParameterPanel further
    down, bound to the focused state's own working accessors (unchanged);
    Control shows neither, silently using Generate's global defaults with no
    dedicated UI surface of its own. Everything through the sub-mode tab row
    lives directly in controlsContent and is ALWAYS fully visible with zero
    scrolling. Layer 5 (whichever of Generate/Sequence/Control/Perform is
    actually active) is the one part of this editor that still scrolls: it's
    tall and varies a lot tab to tab (Sequence's grid+palette+pattern bank
    alone dwarfs Control's stub), and reserving worst-case height for it in a
    genuinely fixed window measured out taller than any real display (a
    2268pt-tall window on a 1440pt-tall screen, measured directly before this
    trade-off was made) -- so it gets its own small internally-scrolling
    region instead, subModeViewport/subModeContent, the same
    Viewport-wrapping-a-content-Component pattern controlsContent itself
    used before this pass. The Playback Style parameter panel
    (playbackStyleParameterPanel) gets the same treatment via
    playbackStyleParameterViewport, for the same reason (its own
    getPreferredHeight() reserves worst-case-across-all-styles row count).
    Loop Length and Transient Sensitivity (Pass 4) went back to plain
    juce::Slider number boxes (IncDecButtons style) instead of RotaryKnob --
    scoped to just these two; RotaryKnob itself is now unused and deleted. */
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
    void updatePitchModeVisibility(); // shows/hides the Time-Stretch-only controls
    void updateManualBpmOverrideVisibility(); // shows/hides the BPM numeric field
    void updateQuantizeTransientsVisibility(); // shows/hides the Grid dropdown
    void updatePerformanceTrimSnapVisibility(); // shows/hides the Trim Snap Grid-resolution dropdown
    void updatePerformanceQuantizeRecallVisibility(); // shows/hides the Quantize Recall note-value dropdown
    void updateControlBaseNoteDisplay(); // refreshes the base-note name label and the slice-range readout
    void updateControlKeyswitchRows(); // refreshes each keyswitch row's assigned-note label + Assign/Click-a-key button text
    int layoutControlsContent (int contentWidth); // Layers 1-4 only (Pass 3) -- always fully visible, never scrolls; returns the total height it needs

    // Recomputes and re-applies the whole window's size (Pass 5). Pitch
    // Mode's and Playback Style's own panel heights now vary depending on
    // which mode/style is selected (mode-aware Pitch Mode sizing in
    // layoutUniversalControlsRow(), per-style Playback Style parameter sizing in
    // layoutPlaybackStyleSection()) -- the window size this editor computed
    // once at construction through Pass 4 no longer holds for every state,
    // so this must be called again whenever either can change (pitchModeSegments/
    // playbackStyleSegments onSelectionChanged), in addition to the
    // constructor's own initial call. contentWidth itself never actually
    // changes -- every panel that defines it (Tempo/Pitch Mode's own
    // compile-time-constant-width columns) is unaffected by mode/style --
    // only the resulting height does, so the window never changes width,
    // only height, when this runs.
    void updateWindowSize();

    // Layer 5 (Pass 3) -- dispatches to whichever of Generate/Sequence/
    // Control/Perform is actually active, laid out inside subModeContent
    // (viewed through subModeViewport, which scrolls when the active tab's
    // content exceeds its fixed visible height -- see subModeViewport's own
    // doc comment). Returns the total height the active tab's own content
    // needs, which becomes subModeContent's size.
    int layoutSubModeContent (int contentWidth);

    // Lays out ONE tab's own controls, starting at local y == startY within
    // subModeContent. All other tabs' controls keep whatever bounds they
    // last had, but stay invisible via updateActiveTabVisibility() below, so
    // stale bounds are harmless. Each returns the total height its own
    // content needs (not including startY).
    int layoutGenerateTab (int contentWidth, int startY); // Generate's own local Slice-Length-vs-Clock timing choice
    int layoutSequenceTab (int contentWidth, int startY); // existing Sequenced-mode flow, extracted verbatim
    int layoutPerformTab (int contentWidth, int startY); // existing Performance-mode flow, extracted verbatim
    int layoutControlTab (int contentWidth, int startY); // base note, slice range readout, Trigger/Gate toggle, keyswitch assignment rows + shared on-screen keyboard

    // Layer 1 (Pass 4) -- Sample only (Load button, status label), part of
    // the single universal layer (Pass 6). A single content-sized
    // SectionPanel. No contentWidth parameter -- unlike every sibling layout
    // function, this one's own natural width feeds into the constructor's
    // one-time contentWidth measurement alongside layoutUniversalControlsRow()'s
    // (see its own doc comment), not the other way around. Returns the total
    // height consumed.
    int layoutTopToolbar (int startY);

    // Universal controls row (Pass 4/6) -- Reset Edits+Undo+Redo, Tempo,
    // Detection, Fade In/Out, Pitch Mode. Two explicit rows of content-sized
    // SectionPanels/button clusters (mirrors the old layoutGlobalSection()'s
    // measure-then-shrink SectionPanel pattern, just laid out side by side
    // instead of stacked in one column; this is what layoutTopToolbar()
    // itself did pre-Pass-4, before Sample split out into its own Layer 1).
    // No contentWidth parameter -- like layoutTopToolbar(), this one's own
    // natural width is part of what DEFINES contentWidth for every layer
    // below it (see the constructor's one-time measurement pass). Shown
    // unconditionally regardless of which sub-mode tab is active (Pass 6:
    // there's no longer a Beats/Textures split to gate it on). Returns the
    // total height consumed.
    int layoutUniversalControlsRow (int startY);

    // Playback Style (selector, probability weight table, parameter panel),
    // sitting directly below layoutUniversalControlsRow() and above the
    // sub-mode tab row. Unlike the universal controls row, its visibility is
    // mode-specific (Pass 6): Generate shows all of it (bound to the global
    // default), Sequence shows just the probability table, and Control/
    // Perform show none of it (Perform has its own separate
    // performanceStyleParameterPanel instead, further down). Laid out
    // unconditionally regardless of the active sub-mode (matching every
    // other layer here that stays laid out even while hidden), but the
    // amount of content -- and so the height returned -- varies with
    // subModeTabs' current selection.
    int layoutPlaybackStyleSection (int contentWidth, int startY);

    // Absolute (controlsContent-local) content rect for a SectionPanel
    // that's already had setBounds() called -- title bar trimmed, standard
    // padding applied, same math as SectionPanel::getContentArea() but
    // translated into controlsContent's coordinate space (children of a
    // SectionPanel's content stay direct children of controlsContent, not
    // of the panel itself -- see SectionPanel's own class doc comment).
    static juce::Rectangle<int> sectionContentArea (const SectionPanel& panel);

    // Shows/hides every top-level page (Generate's sections, Sequence's
    // controls, Control's controls, Perform's controls) plus the
    // mode-specific Playback Style area, based on subModeTabs' current
    // selection -- the tab-driven analogue of
    // updateSliceLengthClockVisibility()'s mode-driven show/hide, called
    // whenever the tab row changes.
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

    // Layers 1-4 (load button through the sub-mode tab row) live inside
    // controlsContent, a plain non-scrolling child -- always fully visible,
    // the "universal, above everything" part of the spec. Layer 5 lives in
    // subModeContent/subModeViewport instead (see their own doc comment
    // below) since it's the one part that still scrolls.
    juce::Component controlsContent;

    // Layer 5 (Pass 3) -- whichever of Generate/Sequence/Control/Perform
    // is active lives in subModeContent, viewed through
    // subModeViewport at a fixed visible height (subModeViewportHeight):
    // the one deliberate scrolling region left in this editor. A genuinely
    // fixed, scroll-free window sized to the tallest tab's worst case
    // measured out taller than any real display (2268pt tall, on a 1440pt
    // screen) -- Sequence's grid+palette+pattern bank alone dwarfs Control's
    // stub, so reserving that much fixed height for every tab wasn't
    // viable. Same Viewport-wrapping-a-content-Component pattern
    // controlsContent itself used before this pass (and the same pattern
    // sequencerViewport/sequencerGrid still use one level deeper).
    juce::Viewport subModeViewport;
    juce::Component subModeContent;
    static constexpr int subModeViewportHeight = 180;

    // Applies the Tungsten/Salmon palette to every ComboBox/ToggleButton/
    // TextButton/Label/Slider still using its native JUCE widget type this
    // pass -- scoped to controlsContent only (see NeditPalette::LookAndFeel's
    // own doc comment for why), applied once in the constructor.
    NeditPalette::LookAndFeel neditLookAndFeel;

    // Sub-mode tab row: Generate/Sequence/Control/Perform, sitting directly
    // beneath the single universal layer now that Textures (and the
    // Beats/Textures tab row above this one) has been removed entirely
    // (Pass 6). Reuses SegmentedButtonRow directly rather than a separate
    // TabBar class -- a tab bar is exactly that component's use case
    // (mutually exclusive, click-to-select, get/set index for
    // poll-and-resync).
    SegmentedButtonRow subModeTabs; // "Generate" / "Sequence" / "Control" / "Perform"

    // Remembers which of Slice Length/Clock Generate's own Trigger Mode row
    // last selected -- processor.getTriggerMode() gets forced to sequenced/
    // performance while the Sequence/Perform tabs are active (see
    // syncTriggerModeToActiveTab()), so returning to the Generate tab needs
    // this to restore the right mode rather than always defaulting back to
    // Slice Length.
    SlicerAudioProcessor::TriggerMode lastGenerateTriggerMode = SlicerAudioProcessor::TriggerMode::sliceLength;

    // Layer 1 section (Pass 4) -- part of the single universal layer
    // (Pass 6), laid out by layoutTopToolbar(), never gated on the active
    // sub-mode tab. Pure visual backdrop; the real controls below stay
    // direct children of controlsContent, positioned inside its
    // getContentArea() (see SectionPanel's own class doc comment for why it
    // doesn't own/reparent them).
    SectionPanel sampleSectionPanel { "Sample" };

    // Universal controls row (Pass 4/6) -- Reset Edits+Undo+Redo, Tempo,
    // Detection, Fade In/Out, Pitch Mode: visible across all four sub-mode
    // tabs unconditionally (e.g. Sequence/Control/Perform all still want to
    // see the sample's tempo/detection/fade/pitch-mode settings even though
    // none of them expose their own copies). Laid out as a self-sizing
    // two-row cluster by layoutUniversalControlsRow(). Pure visual
    // backdrops, same convention as sampleSectionPanel above -- real
    // controls stay direct children of controlsContent. Fade In/Out is its
    // own panel, split out of what used to be a combined "Trim & Tempo"
    // panel.
    SectionPanel trimTempoSectionPanel { "Tempo" };
    SectionPanel detectionSectionPanel { "Detection" };
    SectionPanel fadeSectionPanel { "Fade In/Out" };
    SectionPanel pitchModeSectionPanel { "Pitch Mode" };

    // Playback Style (Pass 3/4/6) -- directly below the universal controls
    // row, laid out by layoutPlaybackStyleSection(). Unlike the universal
    // row above, its visibility is mode-specific: shown (in full or in part)
    // only for Generate/Sequence -- see updateActiveTabVisibility().
    SectionPanel playbackStyleSectionPanel { "Playback Style" };

    // Generate's own local section (Pass 2) -- the only thing left that's
    // actually Generate-specific: which of Slice Length/Clock times the
    // engine, plus Clock's own reference/scope/subdivision controls and
    // Slice Length's mandatory Reset-every selector. Replaces the old
    // 3-way "Trigger Mode" selector (Slice Length/Clock/Sequenced), which
    // was redundant once Sequenced/Performance became reachable only via
    // their own top-level sub-mode tabs.
    SectionPanel timingSectionPanel { "Timing" };

    // Every component that belongs to the universal layer/Generate/Sequence/
    // Perform respectively -- populated once in the constructor (after all
    // are fully constructed) and used purely for blanket setVisible() in
    // updateActiveTabVisibility(), so switching sub-mode tabs doesn't
    // require remembering to individually hide each control one by one.
    // universalComponents (Pass 6: Sample plus what used to be the separate
    // Beats-specific block -- Reset Edits+Undo+Redo, Tempo, Detection, Fade
    // In/Out, Pitch Mode) is shown unconditionally, regardless of which
    // sub-mode tab is active. Playback Style's own components are NOT in
    // this vector -- their visibility is mode-specific (Generate/Sequence
    // only) and set directly in updateActiveTabVisibility() instead.
    // generateComponents is Generate's own local timing section only. Fine-
    // grained sub-visibility WITHIN an active tab (Clock-only controls, Set
    // Interval's own picker, etc.) is still handled by the existing
    // updateSliceLengthClockVisibility()/updatePitchModeVisibility()/etc.
    // functions, called after the blanket show.
    std::vector<juce::Component*> universalComponents;
    std::vector<juce::Component*> generateComponents;
    std::vector<juce::Component*> sequenceComponents;
    std::vector<juce::Component*> performComponents;
    std::vector<juce::Component*> controlComponents;

    juce::TextButton loadButton { "Load Sample..." };
    juce::TextButton resetEditsButton { "Reset edits" };
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::Label statusLabel;

    // Audition (Step 25) — plays [trimStart, trimEnd) on a tight raw loop,
    // independent of host transport, so a trim can be dialled in and
    // confirmed by ear before worrying about the DAW's own playback state.
    // Label toggles between "Audition"/"Stop Audition" in timerCallback()
    // (same polling mechanism already used to keep Undo/Redo's enabled
    // state in sync) since the processor can also stop it on its own the
    // instant host transport starts — the button has to reflect that
    // auto-stop, not just its own clicks.
    juce::TextButton auditionButton { "Audition" };

    juce::Label loopLengthLabel;
    juce::Slider loopLengthSlider; // integer bars, e.g. 1-8 -- plain IncDecButtons number box (Pass 4: back from RotaryKnob, scoped to just this + sensitivitySlider)
    juce::Label calculatedBpmLabel;

    // Loop length staleness flag (Step 33) — Loop Length (bars) drives the
    // bars-derived original-tempo calculation (computeSourceSpanSeconds()),
    // but that calculation only knows the CURRENT trim range, not whether
    // it's still the right bar count for whatever the trim now covers.
    // Trimming doesn't auto-adjust Loop Length (there's no way to guess
    // the right value), so instead this makes the now-possibly-wrong
    // value impossible to miss: set true whenever waveformDisplay reports
    // an actual trim change, cleared the moment the user acknowledges by
    // touching loopLengthSlider at all (even re-entering the same value —
    // the point is acknowledgment, not a real change). Purely a
    // visibility aid; doesn't affect any tempo/audio calculation itself.
    bool loopLengthNeedsAttention = false;

    // Manual BPM override (Step 23) — replaces the bars-derived tempo
    // calculation entirely when enabled, rather than working alongside it.
    // The numeric field is only shown/enabled while the toggle is on, same
    // show/hide pattern as the Time-Stretch-only and Clock-only controls.
    juce::ToggleButton manualBpmOverrideToggle { "Manual BPM override" };
    juce::Label manualBpmOverrideLabel;
    juce::Slider manualBpmOverrideSlider;

    juce::Label pitchModeLabel;
    SegmentedButtonRow pitchModeSegments; // "Repitch" / "Time-Stretch" -- custom-painted, replaces the old juce::ComboBox (Pass 1)

    // Beat-quantized slice length — Repitch mode (Step 26). Same label and
    // concept as the Time-Stretch toggle below, but its own separate
    // control/state (defaults differ: this one's OFF by default, since it
    // trades off pitch accuracy rather than being a free improvement).
    // Repitch-only, shown/hidden opposite the Time-Stretch-only group.
    juce::ToggleButton beatQuantizeToggleRepitch { "Beat-quantize slice length" };

    // Time-Stretch-only controls — same reserved-space/hide pattern as the
    // Clock-mode-only controls below (grain overlap is fixed at 50% and
    // deliberately not exposed here). Repitch mode doesn't get a pitch
    // slider — pitch is already intentionally tied to tempo there.
    // Pass 3 -- viewed through pitchModeExtraViewport at a small fixed
    // height rather than laid out inline at full height alongside Repitch's
    // own single toggle: same worst-case-reservation problem (and same
    // fix) as playbackStyleParameterViewport (see its own doc comment) --
    // Pitch Mode lives in Layer 1's always-visible, non-scrolling toolbar,
    // so reserving all four of these rows permanently (even while Repitch,
    // which needs none of them, is selected) was one of Layer 1's biggest
    // single contributors to its fixed height.
    juce::Viewport pitchModeExtraViewport;
    juce::Component pitchModeExtraContent;
    juce::Label grainSizeLabel;
    juce::Slider grainSizeSlider;
    juce::Label windowShapeLabel;
    juce::ComboBox windowShapeSelector; // "Hann" / "Triangular"

    // Beat-quantized slice length (Step 24) — also Time-Stretch-only, same
    // show/hide group as the rest of this section. Only actually takes
    // effect in Slice Length trigger mode (Clock mode's tick system
    // already enforces beat-alignment), but stays visible/settable
    // regardless of trigger mode, same as grain size/window shape/pitch
    // shift above.
    juce::ToggleButton beatQuantizeToggle { "Beat-quantize slice length" };

    juce::Label pitchShiftLabel;
    juce::Slider pitchShiftSlider; // semitones, -24 to +24

    juce::Label sensitivityLabel;
    juce::Slider sensitivitySlider; // 0.00-1.00 -- plain IncDecButtons number box (Pass 4: back from RotaryKnob, scoped to just this + loopLengthSlider)

    // Quantize detected transients to grid (Step 35) — auto-detected
    // transients only, never manual points (see PluginProcessor.h's
    // doc comment for why). Grid dropdown visible only while the toggle
    // is on, same show/hide-in-reserved-space pattern used throughout
    // this editor.
    juce::ToggleButton quantizeTransientsToggle { "Quantize transients" };
    juce::Label quantizeGridLabel;
    juce::ComboBox quantizeGridSelector; // reuses the same 20-value note-value palette as Clock reference/Beat-quantize

    juce::Label fadeInLabel;
    juce::Slider fadeInSlider;
    juce::Label fadeOutLabel;
    juce::Slider fadeOutSlider;

    juce::Label sliceLengthClockLabel;
    // "Slice Length" / "Clock" -- Generate's own local timing choice
    // (Pass 2), replacing the old 3-way "Trigger Mode" selector. Sequenced
    // and Performance are reached only via their own top-level sub-mode
    // tabs now (see subModeTabs below and syncTriggerModeToActiveTab()), so
    // this row only ever needs to offer the two choices that stay scoped
    // to Generate itself.
    SegmentedButtonRow sliceLengthClockSegments;

    // Playback style (Step 19/21) — Generate-only now (Pass 6): shown
    // alongside the probability table for Generate specifically. Sequence
    // shows the probability table (playbackStyleGrid) alone, without this
    // selector, since Sequence's own Randomize draws from these exact same
    // per-style probability weights (see SequencerGrid's doc comment) but
    // has no need for a persistent style-parameter panel of its own -- its
    // per-step overrides stay in SequencerGrid's right-click popup instead.
    // Which style's PARAMETERS are currently shown in
    // playbackStyleParameterPanel below (Pass 1) -- purely a UI navigation
    // concept, independent of playbackStyleGrid's own per-style trigger
    // probability weights just below (not mutually exclusive -- multiple
    // styles can have nonzero probability at once, so that control can't
    // become this segmented row itself). Coloured per-style via
    // PlaybackStylePalette::getStyleColour() so the two rows read as two
    // views onto the same 9 styles.
    SegmentedButtonRow playbackStyleSegments;

    juce::Label playbackStyleLabel;
    PlaybackStyleGrid playbackStyleGrid;

    // Playback Style parameter panel -- a style picker + a persistent
    // panel of that style's own parameters (Static/Sweep In/Sweep Out
    // modes, sliders, discrete option pickers), all reusing the exact
    // widgets SequencerGrid's right-click menu already uses. Generate-only
    // (Pass 6) -- bound to the global default values. Sequence's own
    // per-step right-click editing (SequencerGrid's popup) remains the only
    // way to set per-step overrides there; edits made here write straight
    // into the same global-default storage a Sequencer step without its own
    // override already falls back to. Pass 3 -- viewed through
    // playbackStyleParameterViewport at a small fixed height rather than
    // laid out inline at its own getPreferredHeight() (which reserves
    // worst-case-across-all-styles row count): lives in the always-visible,
    // non-scrolling controlsContent, so that reservation would otherwise be
    // paid in full, permanently, regardless of which style is selected.
    juce::Label playbackStyleParametersLabel;
    juce::Viewport playbackStyleParameterViewport;
    PlaybackStyleParameterPanel playbackStyleParameterPanel;

    // Clock-mode-only controls — laid out in reserved space, hidden
    // (setVisible(false)) rather than the window resizing dynamically,
    // whenever Slice Length mode is selected.
    juce::Label clockReferenceLabel;
    juce::ComboBox clockReferenceSelector;

    // Tape Stop scope (Step 21) — also Clock-mode-only: Slice Length mode
    // doesn't need this choice, since a Tape Stop pick's duration there is
    // always just the pick's own natural slice length.
    juce::Label tapeStopScopeLabel;
    juce::ComboBox tapeStopScopeSelector; // "Whole window" / "Per tick"

    // Filter Sweep scope (Step 30) — same Clock-mode-only visibility
    // pattern as Tape Stop scope above, but its own separate control/state
    // (defaults differ: this one's Per Tick by default, the opposite of
    // Tape Stop scope's Whole Window default).
    juce::Label filterSweepScopeLabel;
    juce::ComboBox filterSweepScopeSelector; // "Whole window" / "Per tick"

    // Slice Length periodic reset (Step 34) — the mirror image of the
    // Clock-mode-only controls above: visible ONLY in Slice Length mode,
    // since Clock mode already has its own window-boundary mechanism via
    // Clock reference and doesn't need this at all. No "Off" option —
    // mandatory whenever Slice Length mode is active.
    juce::Label resetEveryLabel;
    juce::ComboBox resetEverySelector; // "1 bar" / "2 bars" / "4 bars" / "8 bars"

    juce::Label subdivisionTableLabel;
    SubdivisionProbabilityGrid subdivisionGrid;

    // Sequenced trigger mode (Step 37, v1 -- monophonic) — Sequenced-only,
    // same reserved-space/hide pattern as the Clock-only and Slice-Length-
    // only groups above. Step resolution reuses the same 20-value
    // note-value palette as Clock reference/Beat-quantize/Quantize Grid.
    // sequencerGrid is self-sizing (see its own doc comment) and can be
    // wider/taller than the space available for it, so it lives inside its
    // own nested Viewport (unlike controlsContent itself, which is no
    // longer a Viewport's child as of Pass 3 -- see its own doc comment).
    juce::Label stepResolutionLabel;
    juce::ComboBox stepResolutionSelector;

    // Pattern length (Step 38) -- 1/2/4 bars, deliberately separate from
    // Loop length (bars): that control feeds the loaded audio's tempo
    // calculation and has no reason to match how many bars the drawn
    // pattern spans. Same dropdown-reusing-a-static-name-table pattern as
    // Reset every.
    juce::Label patternLengthLabel;
    juce::ComboBox patternLengthSelector;

    juce::TextButton randomizeSequenceButton { "Randomize Sequence" };

    // Clear Sequence (Step 41) -- wipes the pattern with no generation
    // afterward, sits next to Randomize Sequence in the same row.
    juce::TextButton clearSequenceButton { "Clear Sequence" };

    // Style Palette (Step 41) -- a fixed-width sidebar of colour swatches
    // beside sequencerViewport, one per PlaybackStyle; clicking one sets
    // the processor's currently selected drawing style. sequencerGrid's
    // own target width (set in resized()) is reduced by this component's
    // width so the COMBINED row still lines up with WaveformDisplay's
    // width, same as it did alone before this existed.
    PlaybackStylePalette playbackStylePalette;

    // BankSource adapter -- PatternBankPanel (see its own class doc
    // comment) is generic over what a slot holds; this is the only place
    // that generic interface gets tied back to SlicerAudioProcessor's
    // Sequencer pattern bank. Performance mode's own bank now uses
    // PerformanceKeyboardPanel::Source instead (see below) -- click-to-focus
    // + auto-save, not MIDI Learn, so it isn't a BankSource at all anymore.
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

    // PerformanceKeyboardPanel::Source adapter -- the only place that
    // generic interface gets tied back to SlicerAudioProcessor's
    // Performance mode state bank. focusSlot() is the ENTIRE interaction
    // model now: clicking a key auto-saves whatever was being edited in the
    // previously-focused slot and loads (or creates) the clicked one.
    struct PerformanceKeyboardSource : public PerformanceKeyboardPanel::Source
    {
        explicit PerformanceKeyboardSource (SlicerAudioProcessor& p) : processor (p) {}
        void focusSlot (int noteNumber) override { processor.setFocusedPerformanceStateSlot (noteNumber); }
        std::array<bool, 128> getPopulatedSlots() const override { return processor.getPopulatedPerformanceStateBankSlots(); }
        int getFocusedSlot() const override { return processor.getFocusedPerformanceStateSlot(); }
        SlicerAudioProcessor& processor;
    };


    // MIDI pattern bank (Sequenced mode only) -- populated/active slot
    // indicators plus the "Save to..." MIDI Learn control. See its own
    // class doc comment for the note-layout/interaction details.
    SequencerBankSource sequencerBankSource;
    PatternBankPanel patternBankPanel;

    // Pattern Switch Timing (Pass 2) -- governs WHEN a pattern-bank recall
    // note-on above actually takes effect (Immediate/Set Interval/End of
    // Pattern); sits directly below patternBankPanel since it's purely
    // about that recall's timing, not pattern content itself.
    // patternSwitchIntervalSelector reuses the exact same 20-value
    // note-value palette as clockReferenceSelector/stepResolutionSelector
    // above, and is only shown while Set Interval is selected (same
    // show/hide-by-mode convention as everything else in
    // updateActiveTabVisibility()).
    juce::Label patternSwitchTimingLabel;
    juce::ComboBox patternSwitchTimingSelector;
    juce::Label patternSwitchIntervalLabel;
    juce::ComboBox patternSwitchIntervalSelector;

    // Performance mode -- reuses PlaybackStyleParameterPanel, pointed at
    // Performance mode's own working-state storage instead of the global
    // default values Slice Length/Clock use (see its constructor call in
    // the .cpp), and the on-screen keyboard (PerformanceKeyboardPanel) via
    // performanceKeyboardSource above for click-to-focus + auto-save.
    // Loop/Sync are simple toggles -- no existing component fits either.
    // Own row/section in layoutControlsContent(), visible only in
    // Performance mode (see updateActiveTabVisibility()).
    juce::Label performanceStyleParametersLabel;
    PlaybackStyleParameterPanel performanceStyleParameterPanel;
    juce::ToggleButton performanceLoopToggle { "Loop" };
    juce::ToggleButton performanceSyncToggle { "Sync" };

    // Trim Snap mode -- governs what dragging a state's trim handle snaps
    // to: Transients (existing behaviour) or Grid (fixed musical grid at
    // the sample's established tempo, ignoring detected transients
    // entirely). performanceTrimGridSelector reuses the same note-value
    // palette as Clock reference/Quantize Transients' Grid/Subdivide, and
    // is only shown while Grid is the selected snap mode (same show/hide-
    // by-selection convention as Set Interval's own note-value picker --
    // see updatePerformanceTrimSnapVisibility()).
    juce::Label performanceTrimSnapLabel;
    juce::ComboBox performanceTrimSnapSelector;
    juce::Label performanceTrimGridLabel;
    juce::ComboBox performanceTrimGridSelector;

    // Quantize Recall -- governs WHEN a physical MIDI key press's recall (or
    // live-audition, for the focused key) actually takes effect: off
    // (default) is the original immediate switch; on defers it to the next
    // occurrence of performanceQuantizeRecallIntervalSelector's chosen grid
    // point, same "Set Interval" mechanism Pattern Switch Timing above
    // already uses for the Sequencer pattern bank.
    // performanceQuantizeRecallIntervalSelector reuses the same note-value
    // palette as Clock reference/Set Interval/Trim Snap's own Grid picker,
    // and is only shown while the toggle is on (same show/hide-by-selection
    // convention as those other pickers -- see
    // updatePerformanceQuantizeRecallVisibility()).
    juce::ToggleButton performanceQuantizeRecallToggle { "Quantize Recall" };
    juce::Label performanceQuantizeRecallIntervalLabel;
    juce::ComboBox performanceQuantizeRecallIntervalSelector;

    PerformanceKeyboardSource performanceKeyboardSource;
    PerformanceKeyboardPanel performanceKeyboardPanel;

    // Control mode -- piano-roll slice triggering with keyswitch style
    // selection (see SlicerAudioProcessor::TriggerMode::control's own doc
    // comment for the overall design). Base note is a plain IncDecButtons
    // number box (Pass 4's "back from RotaryKnob" convention, same as Loop
    // Length/Sensitivity), with a live note-name readout since raw MIDI
    // note numbers aren't self-explanatory; the slice-range label is purely
    // informational (derived from base note + slice count, never edited
    // directly). Keyswitch notes are a fixed, hardcoded block below the base
    // note (processor.getControlKeyswitchNote()) -- controlKeyswitchLabels
    // is purely a read-only display, one row per style, refreshed whenever
    // the base note changes; nothing here is clickable or editable.
    juce::Label controlBaseNoteLabel;
    juce::Slider controlBaseNoteSlider;
    juce::Label controlBaseNoteNameLabel;
    juce::Label controlSliceRangeLabel;

    juce::Label controlGateModeLabel;
    SegmentedButtonRow controlGateModeSegments; // "Trigger" / "Gate"

    juce::Label controlKeyswitchSectionLabel;
    std::array<juce::Label, SlicerAudioProcessor::numPlaybackStyleOptions> controlKeyswitchLabels; // e.g. "Forward: B0"

    juce::Viewport sequencerViewport;
    SequencerGrid sequencerGrid;
    static constexpr int sequencerViewportHeight = 200;

    // Zoom/pan (Step 31) — live directly on the editor (like waveformDisplay
    // itself), not inside controlsContent, staying visually adjacent to the
    // waveform they control rather than living up in Layer 1's toolbar with
    // the rest of the universal controls.
    juce::TextButton zoomToTrimsButton { "Zoom to Trims" };
    juce::TextButton resetZoomButton { "Reset Zoom" };

    WaveformDisplay waveformDisplay;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
