#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

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
    shows exactly what will be heard, not just an approximation.

    Self-sizing: polls the processor's current row/column counts on a
    timer (same 30fps live-update pattern WaveformDisplay already uses)
    and resizes itself whenever they change, so it's meant to live inside
    a juce::Viewport with both scrollbars enabled -- the grid can be wider
    and/or taller than any reasonably-sized fixed viewport. The same timer
    also drives the live playhead column highlight. */
class SequencerGrid : public juce::Component,
                       private juce::Timer
{
public:
    explicit SequencerGrid (SlicerAudioProcessor& processorToUse);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    void timerCallback() override; // polls dimensions (resizes if changed) and drives the playhead + repaint

    int getRowIndexAtY (int y) const;
    int getColumnIndexAtX (int x) const;

    // How many subsequent columns (from startColumn, inclusive) a note in
    // `row` should visually span -- see the class doc comment above.
    int computeBarLengthInSteps (int row, int startColumn, int numRows, int numColumns) const;

    SlicerAudioProcessor& processor;

    static constexpr int rowHeight = 16;
    static constexpr int columnWidth = 20;

    // Drag state (Step 37) -- dragRow is locked for the whole gesture
    // (-1 when not dragging); dragIsActivating is decided once, from the
    // FIRST cell's own prior state on mouseDown, and reused for every
    // subsequent cell the drag passes over.
    int dragRow = -1;
    bool dragIsActivating = true;

    // Self-sizing (see class doc comment) -- only calls setSize() when
    // the dimensions actually change, not every tick.
    int lastKnownNumRows = 0;
    int lastKnownNumColumns = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerGrid)
};
