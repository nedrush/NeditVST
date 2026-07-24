#include "SequencerGrid.h"

SequencerGrid::SequencerGrid (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
    lastKnownNumRows = processor.getSequencerNumRows();
    lastKnownNumColumns = processor.getSequencerNumSteps();
    lastKnownTargetWidth = targetWidth;
    setSize (juce::jmax (1, targetWidth), juce::jmax (1, lastKnownNumRows * rowHeight));

    startTimerHz (30); // same live-update cadence WaveformDisplay's own timer already uses
}

void SequencerGrid::setTargetWidth (int width)
{
    targetWidth = juce::jmax (1, width);
    updateSizeIfNeeded();
}

void SequencerGrid::updateSizeIfNeeded()
{
    const int numRows = processor.getSequencerNumRows();
    const int numColumns = processor.getSequencerNumSteps();

    if (numRows != lastKnownNumRows || numColumns != lastKnownNumColumns || targetWidth != lastKnownTargetWidth)
    {
        lastKnownNumRows = numRows;
        lastKnownNumColumns = numColumns;
        lastKnownTargetWidth = targetWidth;
        setSize (juce::jmax (1, targetWidth), juce::jmax (1, numRows * rowHeight));
    }
}

int SequencerGrid::getColumnWidth() const
{
    const int numColumns = processor.getSequencerNumSteps();

    if (numColumns <= 0)
        return targetWidth;

    return juce::jmax (minColumnWidth, targetWidth / numColumns);
}

void SequencerGrid::timerCallback()
{
    updateSizeIfNeeded();
    repaint(); // cheap enough at this grid's typical size to just always repaint, same as WaveformDisplay's own timer
}

int SequencerGrid::getRowIndexAtY (int y) const
{
    const int numRows = processor.getSequencerNumRows();

    if (numRows <= 0)
        return 0;

    // Row 0 (the first slice) renders at the BOTTOM of the grid (Step 38,
    // standard piano-roll convention -- see the class doc comment), so the
    // screen row read from y has to be inverted to get back to the data
    // row index processor.getSequencerCell()/setSequencerCell() expect.
    const int screenRow = juce::jlimit (0, numRows - 1, y / rowHeight);
    return numRows - 1 - screenRow;
}

int SequencerGrid::getColumnIndexAtX (int x) const
{
    const int numColumns = processor.getSequencerNumSteps();

    if (numColumns <= 0)
        return 0;

    return juce::jlimit (0, numColumns - 1, x / getColumnWidth());
}

int SequencerGrid::computeBarLengthInSteps (int row, int startColumn, int numRows, int numColumns) const
{
    const Slice slice = processor.getSlice (row);
    const int sliceLength = slice.lengthInSamples();
    const double sampleRate = processor.getSampleSampleRate();
    const double originalBpm = processor.getCalculatedOriginalBpm();

    int naturalSteps = 1;

    if (sliceLength > 0 && sampleRate > 0.0 && originalBpm > 0.0)
    {
        const double sliceSeconds = (double) sliceLength / sampleRate;
        const double naturalBeats = sliceSeconds * (originalBpm / 60.0);
        const double stepBeats = SlicerAudioProcessor::getNoteValueBeats (processor.getStepResolutionIndex());

        if (stepBeats > 0.0)
            naturalSteps = juce::jmax (1, juce::roundToInt (naturalBeats / stepBeats));
    }

    // Cut short at whichever comes first: the natural length above, or
    // the next active cell anywhere in the grid (any row, not just this
    // one) -- structural monophony means THAT is what will actually cut
    // this note off at playback, so the piano roll always shows exactly
    // what will be heard. No visual wrap-around past the grid's right
    // edge for v1 -- simply clamped there instead.
    int barLength = naturalSteps;

    for (int offset = 1; offset < naturalSteps; ++offset)
    {
        const int checkColumn = startColumn + offset;

        if (checkColumn >= numColumns)
            break;

        bool columnHasActive = false;

        for (int r = 0; r < numRows; ++r)
        {
            if (processor.getSequencerCell (r, checkColumn))
            {
                columnHasActive = true;
                break;
            }
        }

        if (columnHasActive)
        {
            barLength = offset;
            break;
        }
    }

    return juce::jmin (barLength, numColumns - startColumn);
}

void SequencerGrid::paint (juce::Graphics& g)
{
    const int numRows = processor.getSequencerNumRows();
    const int numColumns = processor.getSequencerNumSteps();

    if (numRows <= 0 || numColumns <= 0)
    {
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (12.0f);
        g.drawFittedText ("No slices to sequence yet", getLocalBounds(), juce::Justification::centred, 1);
        return;
    }

    // Cell backgrounds, shaded every beat (not a hardcoded 4 columns --
    // stepsPerBeat varies with Step resolution) so the grid reads as bars
    // of beats regardless of resolution, plus a faint per-cell outline.
    const double stepBeatsForShading = SlicerAudioProcessor::getNoteValueBeats (processor.getStepResolutionIndex());
    const int stepsPerBeatForShading = juce::jmax (1, juce::roundToInt (stepBeatsForShading > 0.0 ? 1.0 / stepBeatsForShading : 4.0));
    const int columnWidth = getColumnWidth();

    for (int row = 0; row < numRows; ++row)
    {
        // Row 0 at the bottom, standard piano-roll convention (Step 38).
        const int screenY = (numRows - 1 - row) * rowHeight;

        for (int col = 0; col < numColumns; ++col)
        {
            const juce::Rectangle<int> cell (col * columnWidth, screenY, columnWidth, rowHeight);
            const bool beatShade = ((col / stepsPerBeatForShading) % 2) == 0;

            g.setColour (beatShade ? juce::Colours::white.withAlpha (0.04f) : juce::Colours::black.withAlpha (0.12f));
            g.fillRect (cell);

            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawRect (cell, 1);
        }
    }

    // Playhead -- current step column, drawn UNDER the active-cell bars
    // so a lit cell under the playhead stays clearly visible.
    const int playingStep = processor.getCurrentlyPlayingStepIndex();

    if (playingStep >= 0 && playingStep < numColumns)
    {
        const juce::Rectangle<int> playheadColumn (playingStep * columnWidth, 0, columnWidth, numRows * rowHeight);
        g.setColour (juce::Colours::dodgerblue.withAlpha (0.25f));
        g.fillRect (playheadColumn);
    }

    // Active cells -- piano-roll bars.
    for (int row = 0; row < numRows; ++row)
    {
        const int screenY = (numRows - 1 - row) * rowHeight; // row 0 at the bottom (Step 38)

        for (int col = 0; col < numColumns; ++col)
        {
            if (! processor.getSequencerCell (row, col))
                continue;

            const int barLengthSteps = computeBarLengthInSteps (row, col, numRows, numColumns);
            const juce::Rectangle<int> bar (col * columnWidth, screenY, barLengthSteps * columnWidth, rowHeight);

            g.setColour (juce::Colours::orange.withAlpha (0.85f));
            g.fillRect (bar.reduced (1));
            g.setColour (juce::Colours::orange);
            g.drawRect (bar.reduced (1), 1.0f);
        }
    }
}

void SequencerGrid::mouseDown (const juce::MouseEvent& event)
{
    if (processor.getSequencerNumRows() <= 0 || processor.getSequencerNumSteps() <= 0)
        return;

    dragRow = getRowIndexAtY (event.y);
    const int col = getColumnIndexAtX (event.x);

    // A whole drag stroke keeps doing whichever of activate/deactivate
    // this FIRST cell's own prior state calls for, so dragging across
    // already-lit cells erases them and dragging across dark ones lights
    // them, rather than flickering as the cursor crosses cell boundaries.
    dragIsActivating = ! processor.getSequencerCell (dragRow, col);
    processor.setSequencerCell (dragRow, col, dragIsActivating);
    repaint();
}

void SequencerGrid::mouseDrag (const juce::MouseEvent& event)
{
    if (dragRow < 0)
        return;

    // Row is locked to wherever the drag started -- "click-drag across
    // cells in A row," not a free paint across the whole grid.
    const int col = getColumnIndexAtX (event.x);
    processor.setSequencerCell (dragRow, col, dragIsActivating);
    repaint();
}

void SequencerGrid::mouseUp (const juce::MouseEvent&)
{
    dragRow = -1;
}
