#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PlaybackStylePalette.h"

//==============================================================================
/** Step-37: mouse-drawable step-sequencer grid for Sequenced Trigger Mode
    (v1, monophonic). One row per available slice
    (SlicerAudioProcessor::getSequencerNumRows(), capped at 32), one
    column per step (SlicerAudioProcessor::getSequencerNumSteps()).

    Click-drag across cells in a row toggles them on/off, standard
    step-sequencer UX -- the row is locked for the whole drag gesture (set
    by wherever the mouse went down), and the WHOLE drag keeps doing
    whichever of activate/deactivate the first cell did, so dragging
    across already-lit cells erases them and dragging across dark ones
    lights them, rather than flickering as the cursor crosses cell
    boundaries. Structural monophony (only one active cell per column,
    grid-wide) is enforced by the processor's setSequencerCell(), not this
    component -- it just reflects whatever state comes back.

    An active cell draws as a bar spanning however many subsequent
    step-columns that row's slice's natural length covers (same
    beats-from-natural-length math Beat-Quantize already uses), cut short
    at whichever comes first: that natural length, or the next active
    cell anywhere in the grid (any row) -- monophony means THAT is what
    will actually cut the note off at playback, so the piano roll always
    shows exactly what will be heard, not just an approximation. Each bar
    is drawn in its own cell's PlaybackStyle colour (Step 41, see
    PlaybackStylePalette::getStyleColour()) -- clicking/dragging an empty
    cell writes the Style Palette's currently selected style; a cell
    already showing a DIFFERENT style gets painted over with the selected
    one; a cell already showing the SAME style toggles off, preserving
    the "click again to remove" behaviour for the common case.

    Row 0 (the first slice) renders at the BOTTOM of the grid, standard
    piano-roll convention -- this is purely a rendering/hit-testing choice
    (getRowIndexAtY() inverts screen row -> data row); the underlying
    row-index-to-slice-index mapping is untouched.

    Width (Step 38): stretches to whatever target width the editor gives
    it via setTargetWidth() -- meant to be called with WaveformDisplay's
    own width, so the grid always spans exactly as wide as the waveform
    below it rather than shrinking to a cramped fixed-pixel-per-column
    default. Column width is therefore computed (targetWidth / numColumns),
    not a fixed constant -- more columns means thinner columns, same as
    any DAW piano roll. Height still self-sizes to numRows * rowHeight and
    is meant to live inside a juce::Viewport for vertical scrolling when
    there are more rows than fit.

    Dynamic row height: rowHeight itself isn't fixed -- it scales against
    setAvailableHeight() (the editor's sequencerViewport height) so that a
    lightly-sliced sample fills the available vertical space with fewer,
    taller rows rather than leaving it mostly empty, while a busy sample
    (many detected slices) keeps rowHeight at its minRowHeight floor and
    relies on the surrounding Viewport's existing scroll behaviour, same as
    before this existed. See computeRowHeight()/minRowHeight's own doc
    comments. Column width/step resolution are untouched by this -- they
    remain governed by Pattern Length and step resolution, not slice count.

    Self-sizing: polls the processor's current row/column counts on a
    timer (same 30fps live-update pattern WaveformDisplay already uses)
    and resizes itself whenever they (or the target width) change. The
    same timer also drives the live playhead column highlight.

    Step parameter editing (Step 45/46, Flow A): right-click (or
    Cmd/Ctrl-click) an active step to pop up a menu of whichever
    parameters that step's own PlaybackStyle actually uses (see
    SlicerAudioProcessor::getApplicableSequencerCellParameters()) -- e.g.
    Filter Down/Up offers Resonance + Filter Type, Ping-Pong/Tape Stop
    offers Curve Shape, Stretch offers Grain Size + Grain Speed, Forward
    offers nothing (no menu at all). Continuous parameters (Resonance,
    Grain Size, Grain Speed) work exactly as Step 45 established: selecting
    one replaces the menu with a horizontal slider overlay drawn over that
    step; a left-click-drag on the slider adjusts the value live, writing
    into that cell's parameter-override map, and releasing the mouse
    closes the slider automatically. Discrete parameters (Filter Type,
    Curve Shape -- a small named list rather than a range, see
    SlicerAudioProcessor::isSequencerCellParameterDiscrete()) instead
    present as a submenu of their own option names; selecting one writes
    straight into the override map with no slider overlay at all, since a
    handful of named choices doesn't need one. Subdivide (Step 47) is the
    one exception to that split: it's discrete (its options are "Off"
    plus the shared note-value palette) but still uses the drag-slider
    overlay, snapped to those named stops rather than an arbitrary value
    -- and, unlike every other entry here, it's offered on EVERY active
    step regardless of style (a general per-step retrigger rate, not tied
    to one PlaybackStyle) -- see
    SlicerAudioProcessor::isSequencerCellParameterSteppedSlider() and the
    unconditional push_back in getApplicableSequencerCellParameters(). A
    small triangle marker in the corner of any cell with at least one
    override (of any parameter) stays visible afterward. While the slider
    overlay is open, it captures
    the WHOLE next left-click gesture (whether that lands on the slider or
    elsewhere, which just cancels/dismisses it) -- normal cell
    click/drag-to-toggle is otherwise completely unaffected, since
    right-click is a distinct gesture juce::MouseEvent::mods.isPopupMenu()
    identifies before any of the ordinary toggle logic runs.

    Step-extension (Pass 1, mechanism only): Shift+drag near an active
    step's own right edge (findExtendTargetAt() -- the rightmost
    extendGrabZonePx pixels of the bar itself, biased inward rather than
    centred on the exact edge pixel) grows it live, purely as a local
    preview (extendRow/extendStartColumn/
    extendLiveLengthSteps) until mouseUp commits it via
    SlicerAudioProcessor::setSequencerCellExtendedLengthSteps(), the same
    "grab, drag, release commits" shape the parameter slider overlay above
    already uses. Dragging can't shrink below the step's own natural length
    (SlicerAudioProcessor::getSequencerNaturalLengthSteps()) -- this pass is
    growth-only, per its own spec. Committing clears any OTHER row's cells
    the newly-claimed span now covers, same per-column monophony rule
    ordinary click-drawing already enforces (see
    SlicerAudioProcessor::setSequencerCell()). The bar itself just renders
    across the full extended span in the cell's existing style colour -- no
    separate visual treatment for the natural-vs-extended portion in this
    pass. Shift-clicking anywhere that ISN'T near an active step's right
    edge is a no-op, rather than falling through to the ordinary
    toggle-draw gesture below. Tape Stop's decel duration in Sequenced mode
    (PluginProcessor::processBlock()) is driven by this exact same declared
    length (SlicerAudioProcessor::getSequencerCellDeclaredLengthSteps()),
    so the bar this class renders and the decel time actually heard can
    never disagree.

    Send-to-bus (Pass 2): two more entries in the same right-click menu,
    "Send to Delay" and "Send to Reverb", offered on every active step
    regardless of style (same "general" status as Subdivide/Volume) --
    independent of each other and of everything else in the menu, so a
    step can send to either bus, both, or neither. Each is itself a
    submenu grouping that bus's own Send Amount alongside optional
    overrides for its live character (Delay: Time, Feedback; Reverb: Size,
    Decay) -- every one of the six is continuous, so all of them reuse the
    exact same slider-overlay mechanism Resonance/Grain Size/etc. already
    use above; only the grouping is new (see
    SlicerAudioProcessor::getSequencerCellParameterName() indices 21-26).
    Send Amount has no global default (0%/unset until a step explicitly
    sets it -- opt-in, not automatic); the character overrides fall back to
    the shared bus's own current live value when unset, and when they ARE
    set, firing that step doesn't reconfigure a new bus instance -- it eases
    the ONE shared, continuously-running Delay/Reverb bus's live parameter
    toward that value over a short ramp (see
    SlicerAudioProcessor::processSendBuses()), so the bus's character can
    audibly drift as different steps fire without ever clicking. */
class SequencerGrid : public juce::Component,
                       private juce::Timer
{
public:
    explicit SequencerGrid (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

    // Sets the total width this component should span -- called by the
    // editor with WaveformDisplay's own width (Step 38), so the two
    // components always visually line up.
    void setTargetWidth (int width);

    // Sets the total vertical space the editor has allocated for this grid
    // (sequencerViewport's own height) -- drives dynamic row-height scaling
    // (see rowHeight's own doc comment below). Called by the editor's
    // layout code alongside setTargetWidth().
    void setAvailableHeight (int height);

private:
    void timerCallback() override; // polls dimensions (resizes if changed) and drives the playhead + repaint
    void updateSizeIfNeeded();

    int getRowIndexAtY (int y) const;
    int getColumnIndexAtX (int x) const;
    int getColumnWidth() const; // targetWidth / numColumns, clamped to a sane minimum

    // Dynamic row-height calculation (see rowHeight's own doc comment) --
    // pure function of numRows and the currently-known availableHeight, no
    // side effects. Returns minRowHeight when numRows*minRowHeight already
    // exceeds availableHeight (busy samples keep the floor + scrolling,
    // unchanged from before this existed) or when numRows <= 0 (avoids a
    // divide-by-zero and there's nothing to scale anyway).
    int computeRowHeight (int numRows) const;

    // How many subsequent columns (from startColumn, inclusive) a note in
    // `row` should visually span -- see the class doc comment above. Reads
    // its desired (pre-conflict-clamp) length via
    // SlicerAudioProcessor::getSequencerCellDeclaredLengthSteps(), shared
    // with the audio thread's Tape Stop duration calculation so the two
    // can never disagree.
    int computeBarLengthInSteps (int row, int startColumn, int numRows, int numColumns) const;

    // Step-extension (Pass 1) -- true (with outRow/outStartColumn set) if
    // (x, y) lands within the grabbable zone (extendGrabZonePx/
    // extendGrabOuterTolerancePx) near an active step's own right edge
    // (its CURRENT rendered bar, natural or already-extended) in whichever
    // row y falls in.
    bool findExtendTargetAt (int x, int y, int& outRow, int& outStartColumn) const;

    // Step parameter editing (Step 45/46).
    void showParameterMenuForCell (int row, int column);
    juce::Rectangle<int> getParameterSliderBounds (int row, int column, int numRows, int numColumns) const;
    void updateEditingValueFromMouseX (int mouseX, const juce::Rectangle<int>& sliderBounds);

    // Index (into SlicerAudioProcessor::getSequencerCellParameterName())
    // of whichever parameter editingParameterName currently names, or -1
    // if it doesn't match any (shouldn't happen in practice) -- looked up
    // by name rather than storing the index directly since the override
    // map itself is keyed by name (Step 46).
    int findEditingParameterIndex() const;

    SlicerAudioProcessor& processor;

    // Row height floor -- whatever used to be the fixed row height before
    // dynamic scaling. Busy samples (many detected slices) still land
    // here, at minRowHeight, and still scroll inside sequencerViewport
    // exactly as before.
    static constexpr int minRowHeight = 16;

    // Current EFFECTIVE row height, recomputed (via computeRowHeight())
    // every time updateSizeIfNeeded() notices numRows or availableHeight
    // has changed -- never written to directly elsewhere. When
    // numRows*minRowHeight fits within availableHeight (few slices), this
    // scales UP to fill the space, one row per slice, no wasted vertical
    // room and no unnecessary scrollbar; otherwise it stays at
    // minRowHeight and sequencerViewport's existing scroll behaviour takes
    // over, unchanged. Every paint()/hit-test calculation that used to
    // reference the old fixed constant now reads this instead.
    int rowHeight = minRowHeight;

    static constexpr int minColumnWidth = 2; // floor so an extreme column count never collapses to 0px-wide cells

    // Target total width (Step 38) -- matches WaveformDisplay's width, set
    // via setTargetWidth(). Starts at a sane placeholder until the editor's
    // first layout call.
    int targetWidth = 400;

    // Total vertical space the editor has allocated for this grid
    // (sequencerViewport's own height), set via setAvailableHeight() --
    // the denominator computeRowHeight() scales against. 0 until the
    // editor's first layout call, which computeRowHeight() treats the same
    // as "doesn't fit" (falls back to minRowHeight) since there's no real
    // space to scale into yet.
    int availableHeight = 0;

    // Drag state -- dragRow is locked for the whole gesture (-1 when not
    // dragging); dragTargetStyle (Step 41) is decided once on mouseDown --
    // -1 (clear) if the first cell already showed the currently selected
    // style, otherwise the selected style index -- and reused for every
    // subsequent cell the drag passes over, same "whole drag does the same
    // thing" UX as before, just style-aware now instead of a plain toggle.
    int dragRow = -1;
    int dragTargetStyle = -1;

    // Step parameter editing state (Step 45) -- editingRow is -1 when no
    // slider overlay is showing. Set once a parameter is chosen from the
    // right-click menu; cleared (closing the overlay) on mouse release or
    // a click that misses the slider's own bounds.
    int editingRow = -1;
    int editingColumn = -1;
    juce::String editingParameterName;
    bool sliderDragging = false;

    // Step-extension drag state (Pass 1) -- extendRow is -1 when no
    // extension gesture is in progress. extendStartColumn is the step
    // being grown (its style/starting column never change during the
    // gesture); extendLiveLengthSteps is the live, uncommitted preview
    // length paint() renders for that one cell in place of the normal
    // computeBarLengthInSteps() result, committed to the processor only on
    // mouseUp.
    int extendRow = -1;
    int extendStartColumn = -1;
    int extendLiveLengthSteps = 1;

    // Grabbable hit-zone for starting a Shift+drag extension, matching
    // Step 45's precedent of a generous, FIXED-pixel hit target rather
    // than pixel-perfect precision on one exact edge line (columns can get
    // very thin at high step counts, same reasoning the parameter slider
    // overlay's own fixed sliderWidth already applies). Biased INWARD --
    // the rightmost extendGrabZonePx pixels of the bar ITSELF, not
    // centred on the edge -- since "grab near the end of the bar" is more
    // intuitive to land than hunting for one exact pixel; a small amount
    // of extendGrabOuterTolerancePx lets aiming slightly past the edge
    // still count too. See findExtendTargetAt().
    static constexpr int extendGrabZonePx = 18;
    static constexpr int extendGrabOuterTolerancePx = 4;

    // Self-sizing (see class doc comment) -- only calls setSize() when
    // the dimensions actually change, not every tick.
    int lastKnownNumRows = 0;
    int lastKnownNumColumns = 0;
    int lastKnownTargetWidth = 0;
    int lastKnownAvailableHeight = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerGrid)
};
