#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "SubdivisionProbabilityGrid.h"
#include "PlaybackStyleGrid.h"
#include "SequencerGrid.h"

//==============================================================================
/** Step-38 editor: load button, reset-edits safety net, undo/redo, an
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
    Down / Filter Up, rolled once per pick regardless of trigger mode —
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
    arbitrarily far from the beat grid) — all of which live
    inside a fixed-height, internally-scrolling controlsViewport now,
    rather than growing the window every time another control gets added
    — and, below a "Zoom to Trims"/"Reset Zoom" button pair (Step 31), the
    (now wider) waveform display, which stays outside that viewport,
    always fully visible below it: it owns slice visualization, drag-and-
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
    editor has scroll-wheel input disabled, so scrolling controlsViewport
    with the cursor over a slider scrolls the view instead of nudging the
    slider's value. */
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
    void updateTriggerModeVisibility(); // shows/hides the Clock-only controls
    void updatePitchModeVisibility(); // shows/hides the Time-Stretch-only controls
    void updateManualBpmOverrideVisibility(); // shows/hides the BPM numeric field
    void updateQuantizeTransientsVisibility(); // shows/hides the Grid dropdown
    int layoutControlsContent (int contentWidth); // lays out every control below; returns the total height they need

    SlicerAudioProcessor& processor;

    // Step 20: every control below (load button through the subdivision
    // grid/undo-redo row) lives inside controlsContent, which is sized to
    // whatever total height its rows actually need and scrolls inside
    // controlsViewport — a FIXED-height window, regardless of how many
    // controls end up living inside it. Keeps the window itself from
    // growing every time another control gets added; only WaveformDisplay
    // sits outside it, always fully visible.
    juce::Viewport controlsViewport;
    juce::Component controlsContent;
    static constexpr int controlsViewportHeight = 420;

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
    juce::Slider loopLengthSlider; // integer bars, e.g. 1-8
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
    juce::ComboBox pitchModeSelector; // "Repitch" / "Time-Stretch"

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
    juce::Slider sensitivitySlider;

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

    juce::Label triggerModeLabel;
    juce::ComboBox triggerModeSelector; // "Slice Length" / "Clock"

    // Playback style (Step 19/21) — visible in both Slice Length and Clock
    // modes, unlike the Clock-only controls below: it's rolled once per
    // pick in Slice Length mode too, not just once per Clock window.
    // Hidden in Sequenced mode (Step 37) — deliberately deferred there,
    // see SequencerGrid's doc comment.
    juce::Label playbackStyleLabel;
    PlaybackStyleGrid playbackStyleGrid;

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
    // wider/taller than controlsContent, so it lives inside its own
    // nested Viewport, same pattern as controlsViewport/controlsContent
    // itself.
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

    juce::Viewport sequencerViewport;
    SequencerGrid sequencerGrid;
    static constexpr int sequencerViewportHeight = 200;

    // Zoom/pan (Step 31) — live directly on the editor (like waveformDisplay
    // itself), not inside controlsContent, so they stay visible next to the
    // waveform regardless of how far the controls above are scrolled.
    juce::TextButton zoomToTrimsButton { "Zoom to Trims" };
    juce::TextButton resetZoomButton { "Reset Zoom" };

    WaveformDisplay waveformDisplay;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
