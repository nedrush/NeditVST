#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"
#include "SubdivisionProbabilityGrid.h"
#include "PlaybackStyleGrid.h"

//==============================================================================
/** Step-24 editor: load button, reset-edits safety net, undo/redo, status
    label, loop-length/sensitivity controls (with a live preview while
    dragging sensitivity), a manual BPM override toggle + field that
    replaces the bars-derived tempo calculation entirely when enabled,
    fade controls, pitch mode (Repitch vs Time-Stretch, with its grain
    size/window shape/beat-quantize-slice-length/pitch shift controls —
    beat-quantize defaults ON whenever Time-Stretch is active, snapping
    each Forward/Ping-Pong pick's rendered duration to the nearest note
    value so consecutive picks always land on exact beat-grid positions),
    playback style (Forward / Ping-Pong / Tape Stop / Stretch, rolled once
    per pick regardless of trigger mode — Stretch always renders through
    the granular engine regardless of pitch mode, with its own hardcoded
    small-grain/hard-edged-window character, no UI of its own), trigger
    mode (Slice Length vs Clock, with its clock-reference menu, Tape Stop
    scope selector, and subdivision probability grid) — all of which live
    inside a fixed-height, internally-scrolling controlsViewport now,
    rather than growing the window every time another control gets added
    — and the waveform display, which stays outside that viewport, always
    fully visible below it: it owns slice visualization, drag-and-drop
    loading, per-slice probability, manual slice add/move/remove, deleting
    auto-detected transients, a live playhead highlight, and modifier-key
    hover cues. */
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
    static constexpr int controlsViewportHeight = 420; // fixed regardless of content height — this is what caps the window

    juce::TextButton loadButton { "Load Sample..." };
    juce::TextButton resetEditsButton { "Reset edits" };
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };
    juce::Label statusLabel;

    juce::Label loopLengthLabel;
    juce::Slider loopLengthSlider; // integer bars, e.g. 1-8
    juce::Label calculatedBpmLabel;

    // Manual BPM override (Step 23) — replaces the bars-derived tempo
    // calculation entirely when enabled, rather than working alongside it.
    // The numeric field is only shown/enabled while the toggle is on, same
    // show/hide pattern as the Time-Stretch-only and Clock-only controls.
    juce::ToggleButton manualBpmOverrideToggle { "Manual BPM override" };
    juce::Label manualBpmOverrideLabel;
    juce::Slider manualBpmOverrideSlider;

    juce::Label pitchModeLabel;
    juce::ComboBox pitchModeSelector; // "Repitch" / "Time-Stretch"

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

    juce::Label fadeInLabel;
    juce::Slider fadeInSlider;
    juce::Label fadeOutLabel;
    juce::Slider fadeOutSlider;

    juce::Label triggerModeLabel;
    juce::ComboBox triggerModeSelector; // "Slice Length" / "Clock"

    // Playback style (Step 19/21) — visible in BOTH trigger modes, unlike
    // the Clock-only controls below: it's rolled once per pick in Slice
    // Length mode too, not just once per Clock window.
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

    juce::Label subdivisionTableLabel;
    SubdivisionProbabilityGrid subdivisionGrid;

    WaveformDisplay waveformDisplay;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessorEditor)
};
