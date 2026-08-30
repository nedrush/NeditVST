#include "SequencerGrid.h"

namespace
{
    // Right-click menu item ID encoding (Step 46): PopupMenu::showMenuAsync
    // hands back a single flat item ID regardless of which submenu (if
    // any) it came from, so continuous parameters (which open the slider
    // overlay) and discrete parameters' individual options (which write
    // straight to the override map) need to be distinguishable from that
    // one ID alone. Continuous parameter p -> continuousItemIdBase + p;
    // discrete parameter p's option o -> discreteItemIdBase + p*stride + o.
    // Ranges never overlap as long as numSequencerCellParameters stays
    // well under discreteItemStride and no discrete parameter ever gets
    // anywhere near that many options -- true today and for the
    // foreseeable future of this menu.
    constexpr int continuousItemIdBase = 1000;
    constexpr int discreteItemIdBase = 2000;
    constexpr int discreteItemStride = 100;

    int continuousItemId (int paramIndex) { return continuousItemIdBase + paramIndex; }
    int discreteItemId (int paramIndex, int optionIndex) { return discreteItemIdBase + paramIndex * discreteItemStride + optionIndex; }

    bool decodeContinuousItemId (int id, int& paramIndex)
    {
        if (id < continuousItemIdBase || id >= discreteItemIdBase)
            return false;

        paramIndex = id - continuousItemIdBase;
        return true;
    }

    bool decodeDiscreteItemId (int id, int& paramIndex, int& optionIndex)
    {
        if (id < discreteItemIdBase)
            return false;

        const int offset = id - discreteItemIdBase;
        paramIndex = offset / discreteItemStride;
        optionIndex = offset % discreteItemStride;
        return true;
    }
}

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

void SequencerGrid::setAvailableHeight (int height)
{
    availableHeight = juce::jmax (0, height);
    updateSizeIfNeeded();
}

int SequencerGrid::computeRowHeight (int numRows) const
{
    if (numRows <= 0)
        return minRowHeight;

    if (numRows * minRowHeight <= availableHeight)
        return availableHeight / numRows; // scale up to fill the space -- fewer slices, taller rows

    return minRowHeight; // doesn't fit -- floor + sequencerViewport's existing scroll behaviour, unchanged
}

void SequencerGrid::updateSizeIfNeeded()
{
    const int numRows = processor.getSequencerNumRows();
    const int numColumns = processor.getSequencerNumSteps();

    if (numRows != lastKnownNumRows || numColumns != lastKnownNumColumns
        || targetWidth != lastKnownTargetWidth || availableHeight != lastKnownAvailableHeight)
    {
        lastKnownNumRows = numRows;
        lastKnownNumColumns = numColumns;
        lastKnownTargetWidth = targetWidth;
        lastKnownAvailableHeight = availableHeight;
        rowHeight = computeRowHeight (numRows);
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

#if JUCE_DEBUG
    // TEMPORARY DEBUG -- remove once step-extension Tape Stop testing is
    // done. This UI-thread timer is where the audio thread's Tape Stop
    // diagnostic mailbox actually gets printed -- see
    // SlicerAudioProcessor::drainDebugTapeStopEvents()'s own doc comment
    // for why the printing itself can't happen on the audio thread.
    processor.drainDebugTapeStopEvents();
    processor.drainDebugStretchEvents();
#endif
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
    // Natural (Step-resolution-quantized) length, or the Step-extension
    // override if longer -- read from the processor rather than computed
    // locally, so this bar and Tape Stop's decel duration in Sequenced
    // mode (PluginProcessor::processBlock(), via the same
    // getSequencerCellDeclaredLengthSteps()) can never disagree.
    const int desiredSteps = processor.getSequencerCellDeclaredLengthSteps (row, startColumn);

    // Cut short at whichever comes first: the desired length above, or
    // the next active cell anywhere in the grid (any row, not just this
    // one) -- structural monophony means THAT is what will actually cut
    // this note off at playback, so the piano roll always shows exactly
    // what will be heard. No visual wrap-around past the grid's right
    // edge for v1 -- simply clamped there instead.
    int barLength = desiredSteps;

    for (int offset = 1; offset < desiredSteps; ++offset)
    {
        const int checkColumn = startColumn + offset;

        if (checkColumn >= numColumns)
            break;

        bool columnHasActive = false;

        for (int r = 0; r < numRows; ++r)
        {
            if (processor.getSequencerCellStyle (r, checkColumn) >= 0)
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

bool SequencerGrid::findExtendTargetAt (int x, int y, int& outRow, int& outStartColumn) const
{
    const int numRows = processor.getSequencerNumRows();
    const int numColumns = processor.getSequencerNumSteps();

    if (numRows <= 0 || numColumns <= 0)
        return false;

    const int row = getRowIndexAtY (y);
    const int columnWidth = getColumnWidth();

    for (int col = 0; col < numColumns; ++col)
    {
        if (processor.getSequencerCellStyle (row, col) < 0)
            continue;

        const int barLength = computeBarLengthInSteps (row, col, numRows, numColumns);
        const int barStartX = col * columnWidth;
        const int rightEdgeX = barStartX + barLength * columnWidth;

        // Biased inward -- the rightmost extendGrabZonePx pixels of the
        // bar ITSELF, not centred on the exact edge pixel -- plus a little
        // outward tolerance for aiming slightly past it. Clamped so a bar
        // shorter than the zone never reaches back past its own start
        // (which would otherwise bleed into whatever precedes it).
        const int zoneStart = juce::jmax (barStartX, rightEdgeX - extendGrabZonePx);
        const int zoneEnd = rightEdgeX + extendGrabOuterTolerancePx;

        if (x >= zoneStart && x <= zoneEnd)
        {
            outRow = row;
            outStartColumn = col;
            return true;
        }
    }

    return false;
}

juce::Rectangle<int> SequencerGrid::getParameterSliderBounds (int row, int column, int numRows, int numColumns) const
{
    const int columnWidth = getColumnWidth();
    const int screenY = (numRows - 1 - row) * rowHeight;
    const int cellCenterX = column * columnWidth + columnWidth / 2;

    // Fixed pixel width regardless of columnWidth (Step 45) -- a single
    // step is usually far too narrow to drag precisely, especially at
    // higher step resolutions/pattern lengths, so this is drawn "over
    // that step" in the sense of being centred there, not literally
    // constrained to its own cell width.
    constexpr int sliderWidth = 120;
    const int maxX = juce::jmax (0, numColumns * columnWidth - sliderWidth);
    const int sliderX = juce::jlimit (0, maxX, cellCenterX - sliderWidth / 2);

    return { sliderX, screenY, sliderWidth, rowHeight };
}

int SequencerGrid::findEditingParameterIndex() const
{
    for (int i = 0; i < SlicerAudioProcessor::numSequencerCellParameters; ++i)
        if (SlicerAudioProcessor::getSequencerCellParameterName (i) == editingParameterName)
            return i;

    return -1;
}

void SequencerGrid::updateEditingValueFromMouseX (int mouseX, const juce::Rectangle<int>& sliderBounds)
{
    const float t = sliderBounds.getWidth() > 0
        ? juce::jlimit (0.0f, 1.0f, (float) (mouseX - sliderBounds.getX()) / (float) sliderBounds.getWidth())
        : 0.0f;

    const int paramIndex = findEditingParameterIndex();

    if (SlicerAudioProcessor::isSequencerCellParameterSteppedSlider (paramIndex))
    {
        // Subdivide (Step 47): same drag gesture as every other slider
        // overlay, but snapped to one of its named stops (Off + the
        // shared note-value palette) rather than an arbitrary value --
        // the stored override is that stop's OPTION INDEX, not a
        // min/max-mapped float.
        const int numOptions = SlicerAudioProcessor::getSequencerCellParameterNumOptions (paramIndex);
        const int stepIndex = numOptions > 1
            ? juce::jlimit (0, numOptions - 1, juce::roundToInt (t * (float) (numOptions - 1)))
            : 0;
        processor.setSequencerCellParameterOverride (editingRow, editingColumn, editingParameterName, (float) stepIndex);
        return;
    }

    // Generalized per-parameter range (Step 46) -- was hardcoded to
    // Resonance's own range back when it was the only continuous
    // parameter this overlay ever showed.
    const float minValue = SlicerAudioProcessor::getSequencerCellParameterMin (paramIndex);
    const float maxValue = SlicerAudioProcessor::getSequencerCellParameterMax (paramIndex);
    const float value = minValue + t * (maxValue - minValue);

    processor.setSequencerCellParameterOverride (editingRow, editingColumn, editingParameterName, value);
}

void SequencerGrid::showParameterMenuForCell (int row, int column)
{
    const int style = processor.getSequencerCellStyle (row, column);

    // Which parameters (if any) this step's own style actually uses
    // (Step 46) -- generalizes the v1 "only Filter Down/Up" hardcoded
    // check into a per-style table (see PluginProcessor.cpp). Empty means
    // this style has nothing to configure (e.g. Forward, or an empty
    // cell), so right-clicking it is a no-op, same as before.
    const auto applicableParams = SlicerAudioProcessor::getApplicableSequencerCellParameters (style);

    if (applicableParams.empty())
        return;

    juce::PopupMenu menu;

    for (int paramIndex : applicableParams)
    {
        const juce::String name = SlicerAudioProcessor::getSequencerCellParameterName (paramIndex);

        if (SlicerAudioProcessor::isSequencerCellParameterSwept (paramIndex))
        {
            // Swept parameters (Step 49): Sample Rate Reduction/Bit Depth
            // present as a submenu of their own Static/Sweep In/Sweep Out
            // MODE choices first, reusing the exact same discrete-submenu
            // machinery as Filter Type/Curve Shape just below -- the mode
            // is itself an ordinary discrete parameter (paramIndex+1, its
            // hidden pair index), it's just never listed in applicableParams
            // directly. Picking a mode writes it via the SAME
            // decodeDiscreteItemId branch below as any other discrete
            // choice; that branch additionally opens the slider overlay
            // for this value index once it detects a swept-mode pick.
            juce::PopupMenu submenu;
            const int modeParamIndex = paramIndex + 1;
            const int numModes = SlicerAudioProcessor::getSequencerCellParameterNumOptions (modeParamIndex);

            for (int option = 0; option < numModes; ++option)
                submenu.addItem (discreteItemId (modeParamIndex, option),
                                  SlicerAudioProcessor::getSequencerCellParameterOptionName (modeParamIndex, option));

            menu.addSubMenu (name, submenu);
        }
        else if (SlicerAudioProcessor::isSequencerCellParameterDiscrete (paramIndex)
            && ! SlicerAudioProcessor::isSequencerCellParameterSteppedSlider (paramIndex))
        {
            // Discrete parameters (Filter Type, Curve Shape -- Step 46)
            // present as a submenu of their own small option list, picked
            // directly -- a slider overlay doesn't make sense for a
            // handful of named choices. Subdivide (Step 47) is discrete
            // too but explicitly excluded here -- see
            // isSequencerCellParameterSteppedSlider()'s doc comment for
            // why it takes the slider branch below instead.
            juce::PopupMenu submenu;
            const int numOptions = SlicerAudioProcessor::getSequencerCellParameterNumOptions (paramIndex);

            for (int option = 0; option < numOptions; ++option)
                submenu.addItem (discreteItemId (paramIndex, option),
                                  SlicerAudioProcessor::getSequencerCellParameterOptionName (paramIndex, option));

            menu.addSubMenu (name, submenu);
        }
        else
        {
            // Continuous parameters (Resonance, Grain Size, Grain Speed)
            // AND Subdivide (Step 47, stepped-but-slider) both open the
            // same drag-slider overlay -- updateEditingValueFromMouseX()
            // is what tells them apart.
            menu.addItem (continuousItemId (paramIndex), name);
        }
    }

    // Send-to-bus (Pass 2) -- two independent, always-offered submenus (any
    // active step regardless of style, same "general" status as Subdivide/
    // Volume just handled by the loop above -- see
    // getApplicableSequencerCellParameters()'s own doc comment for why
    // indices 21-26 are deliberately excluded from applicableParams itself
    // rather than looped over like everything else here). Each submenu
    // groups its own Send Amount alongside that bus's character overrides
    // (Delay: Time/Feedback; Reverb: Size/Decay) -- every entry is
    // continuous, so all six reuse the exact same continuousItemId()/
    // slider-overlay path as Resonance/Grain Size/etc. above; only the
    // menu's own grouping is new. A step can pick either submenu's items,
    // both, or neither -- entirely independent of one another and of
    // whatever style-specific/Subdivide/Volume entries are already in
    // `menu`.
    juce::PopupMenu delaySendSubmenu;
    delaySendSubmenu.addItem (continuousItemId (21), "Send Amount");
    delaySendSubmenu.addItem (continuousItemId (22), "Time");
    delaySendSubmenu.addItem (continuousItemId (23), "Feedback");
    menu.addSubMenu ("Send to Delay", delaySendSubmenu);

    juce::PopupMenu reverbSendSubmenu;
    reverbSendSubmenu.addItem (continuousItemId (24), "Send Amount");
    reverbSendSubmenu.addItem (continuousItemId (25), "Size");
    reverbSendSubmenu.addItem (continuousItemId (26), "Decay");
    menu.addSubMenu ("Send to Reverb", reverbSendSubmenu);

    // Positioning: without an explicit parent component, JUCE positions
    // (and flips up/down, left/right based on available space) this menu
    // against the whole physical screen/display -- fine for a normal
    // desktop app, but a plugin editor is only ever a small region of
    // that display (often embedded, sometimes with a host window whose
    // true on-screen position JUCE's own display query can't see), so a
    // menu near an edge could still open "off" the visible/interactive
    // area even though it's technically still inside SOME monitor. Giving
    // it the plugin editor's own top-level component as its parent (the
    // editor's actual on-screen window, 900x780 per PluginEditor.cpp's
    // setSize()) makes JUCE run that exact same available-space/flip
    // calculation against THAT area instead of the raw screen (see
    // calculateWindowPos()/getParentArea() in JUCE's own
    // juce_PopupMenu.cpp) -- opening upward instead of downward, or
    // leftward instead of rightward, whenever the default direction
    // wouldn't fit within the editor window. Deliberately NOT
    // sequencerViewport/sequencerGrid itself -- the viewport is only
    // sequencerViewportHeight (200px) tall, and JUCE's own PopupMenu
    // layout (insertColumnBreaks() in juce_PopupMenu.cpp) reflows any
    // menu too tall to fit its available height into multiple columns
    // rather than leaving it as a single scrolling list -- Rate's 20-item
    // list doesn't remotely fit in 200px, so constraining to the viewport
    // turned it into a squashed multi-column grid instead of the normal
    // narrow list every other (2-3 item) menu here still showed
    // correctly. The full editor window has ample height for that not to
    // happen, while still keeping the menu inside the plugin's own
    // visible bounds. getTopLevelComponent() never returns null (it walks
    // up to the root and returns `this` itself if there's no parent yet),
    // so no separate fallback is needed here. Every sub-menu added above
    // (Filter Type/Curve Shape/Rate/the swept-parameter mode pickers)
    // inherits this same parent/positioning automatically --
    // PopupMenu::Options::forSubmenu() carries it forward unchanged, only
    // clearing the target COMPONENT -- so this needs setting in exactly
    // one place, not once per menu level.
    const auto menuOptions = juce::PopupMenu::Options()
        .withParentComponent (getTopLevelComponent());

    // Async, not the blocking show() -- this is a juce::Component's own
    // mouseDown handler, and a modal menu call there would reenter the
    // message loop from inside event dispatch.
    menu.showMenuAsync (menuOptions, [this, row, column] (int result)
    {
        if (result <= 0)
            return; // dismissed without choosing anything

        int paramIndex = -1;
        int optionIndex = -1;

        if (decodeDiscreteItemId (result, paramIndex, optionIndex))
        {
            // Discrete choice (Step 46) -- writes straight into the
            // override map. Every discrete parameter but swept Mode
            // indices (Step 49) stops there, no slider overlay involved at
            // all; a swept Mode choice instead falls through to open the
            // slider for its paired VALUE index (paramIndex-1, by the same
            // index+1-is-the-mode convention showParameterMenuForCell()
            // built the submenu with) -- same "picking a mode configures
            // its amount next" flow regardless of which mode was picked,
            // including Static (still needs its fixed value set).
            processor.setSequencerCellParameterOverride (row, column,
                SlicerAudioProcessor::getSequencerCellParameterName (paramIndex), (float) optionIndex);

            if (SlicerAudioProcessor::isSequencerCellParameterSwept (paramIndex - 1))
            {
                editingRow = row;
                editingColumn = column;
                editingParameterName = SlicerAudioProcessor::getSequencerCellParameterName (paramIndex - 1);
            }

            repaint();
            return;
        }

        if (decodeContinuousItemId (result, paramIndex))
        {
            // Right-clicking a step that already has a marker re-opens the
            // slider at its existing override rather than resetting to
            // default (Step 45) -- this needs no special handling here: the
            // slider's own paint() below always reads the CURRENT override
            // (or the global default if none exists) live, every repaint.
            editingRow = row;
            editingColumn = column;
            editingParameterName = SlicerAudioProcessor::getSequencerCellParameterName (paramIndex);
            repaint();
        }
    });
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
            const int style = processor.getSequencerCellStyle (row, col);

            if (style < 0)
                continue;

            // Step-extension (Pass 1): while this exact cell is the target
            // of an in-progress Shift+drag, show the live uncommitted
            // preview length instead of the processor's still-unchanged
            // stored value.
            const int barLengthSteps = (row == extendRow && col == extendStartColumn)
                ? extendLiveLengthSteps
                : computeBarLengthInSteps (row, col, numRows, numColumns);
            const juce::Rectangle<int> bar (col * columnWidth, screenY, barLengthSteps * columnWidth, rowHeight);
            const auto colour = PlaybackStylePalette::getStyleColour (style);

            g.setColour (colour.withAlpha (0.85f));
            g.fillRect (bar.reduced (1));
            g.setColour (colour);
            g.drawRect (bar.reduced (1), 1.0f);

            // Small triangle marker (Step 45) in the cell's own top-right
            // corner -- the STARTING cell, not smeared across the whole
            // bar -- whenever it has at least one parameter override.
            if (processor.getSequencerCellHasAnyParameterOverride (row, col))
            {
                const float markerSize = (float) juce::jmin (columnWidth, rowHeight) * 0.4f;
                const float right = (float) (col * columnWidth + columnWidth) - 1.0f;
                const float top = (float) screenY + 1.0f;

                juce::Path marker;
                marker.addTriangle (right - markerSize, top, right, top, right, top + markerSize);
                g.setColour (juce::Colours::white);
                g.fillPath (marker);
            }
        }
    }

    // Parameter-editing slider overlay (Step 45/46) -- drawn last, on top
    // of everything else, while a right-click's chosen CONTINUOUS
    // parameter is being adjusted (or just sitting there showing its
    // current value, before any drag has started). Discrete parameters
    // (Filter Type, Curve Shape) never open this overlay at all -- see
    // showParameterMenuForCell().
    if (editingRow >= 0)
    {
        if (editingRow < numRows && editingColumn >= 0 && editingColumn < numColumns)
        {
            const auto sliderBounds = getParameterSliderBounds (editingRow, editingColumn, numRows, numColumns);
            const int paramIndex = findEditingParameterIndex();
            const float currentValue = processor.getSequencerCellParameterOverride (
                editingRow, editingColumn, editingParameterName, processor.getSequencerCellParameterGlobalValue (paramIndex));

            float t = 0.0f;
            juce::String valueText;

            if (SlicerAudioProcessor::isSequencerCellParameterSteppedSlider (paramIndex))
            {
                // Subdivide (Step 47): fill fraction and label come from
                // the snapped OPTION INDEX (Off, 1/4, 1/8, ...), not a
                // min/max-mapped float -- same source of truth
                // updateEditingValueFromMouseX() writes into the override.
                const int numOptions = SlicerAudioProcessor::getSequencerCellParameterNumOptions (paramIndex);
                const int stepIndex = juce::jlimit (0, juce::jmax (0, numOptions - 1), juce::roundToInt (currentValue));
                t = numOptions > 1 ? (float) stepIndex / (float) (numOptions - 1) : 0.0f;
                valueText = SlicerAudioProcessor::getSequencerCellParameterOptionName (paramIndex, stepIndex);
            }
            else
            {
                const float minValue = SlicerAudioProcessor::getSequencerCellParameterMin (paramIndex);
                const float maxValue = SlicerAudioProcessor::getSequencerCellParameterMax (paramIndex);
                const float range = maxValue - minValue;
                t = range > 0.0f
                    ? juce::jlimit (0.0f, 1.0f, (currentValue - minValue) / range)
                    : 0.0f;
                valueText = juce::String (currentValue, 2);
            }

            g.setColour (juce::Colours::black.withAlpha (0.9f));
            g.fillRect (sliderBounds);

            const auto fillBounds = sliderBounds.withWidth ((int) ((float) sliderBounds.getWidth() * t)).reduced (2);
            g.setColour (juce::Colours::cyan);
            g.fillRect (fillBounds);

            g.setColour (juce::Colours::white);
            g.drawRect (sliderBounds, 1);
            g.setFont (10.0f);
            g.drawFittedText (editingParameterName + ": " + valueText,
                               sliderBounds.reduced (2), juce::Justification::centred, 1);
        }
        else
        {
            // Defensive -- grid dimensions shrank out from under the
            // editor (e.g. Step resolution/Pattern length changed while
            // the slider was open). Can't repaint() from inside paint()
            // itself; the next timer tick's own repaint will simply stop
            // drawing an out-of-range overlay from here on.
            editingRow = -1;
        }
    }
}

void SequencerGrid::mouseDown (const juce::MouseEvent& event)
{
    if (processor.getSequencerNumRows() <= 0 || processor.getSequencerNumSteps() <= 0)
        return;

    // Right-click / Cmd-Ctrl-click (Step 45) -- a completely separate
    // gesture from ordinary left-click toggling, checked first so it
    // never falls through to the toggle logic below.
    if (event.mods.isPopupMenu())
    {
        showParameterMenuForCell (getRowIndexAtY (event.y), getColumnIndexAtX (event.x));
        return;
    }

    // While the parameter slider overlay is open (Step 45), this whole
    // gesture belongs to it: a click inside its bounds starts adjusting
    // the value; a click anywhere else just dismisses the overlay without
    // changing anything, rather than also toggling whatever cell was
    // underneath it.
    if (editingRow >= 0)
    {
        const int numRows = processor.getSequencerNumRows();
        const int numColumns = processor.getSequencerNumSteps();
        const auto sliderBounds = getParameterSliderBounds (editingRow, editingColumn, numRows, numColumns);

        if (sliderBounds.contains (event.x, event.y))
        {
            updateEditingValueFromMouseX (event.x, sliderBounds);
            sliderDragging = true;
        }
        else
        {
            editingRow = -1;
        }

        repaint();
        return;
    }

    // Step-extension (shift+drag, Pass 1) -- a completely separate gesture
    // from ordinary click-drag toggling, same "checked first, never falls
    // through" pattern the right-click/slider-overlay branches above
    // already use. Missing the edge is a no-op rather than falling through
    // to an accidental toggle underneath the grab attempt.
    if (event.mods.isShiftDown())
    {
        int row = -1, startColumn = -1;

        if (findExtendTargetAt (event.x, event.y, row, startColumn))
        {
            extendRow = row;
            extendStartColumn = startColumn;
            extendLiveLengthSteps = computeBarLengthInSteps (row, startColumn, processor.getSequencerNumRows(), processor.getSequencerNumSteps());
        }

        return;
    }

    dragRow = getRowIndexAtY (event.y);
    const int col = getColumnIndexAtX (event.x);
    const int selectedStyle = processor.getSelectedDrawingStyle();
    const int existingStyle = processor.getSequencerCellStyle (dragRow, col);

    // A whole drag stroke keeps doing whichever this FIRST cell decided
    // (Step 41): toggle off (-1) if it already showed the currently
    // selected style, otherwise paint the selected style over it --
    // whether that cell was empty or showed a DIFFERENT style. Reused for
    // every subsequent cell the drag passes over, same as the plain
    // toggle this replaced.
    dragTargetStyle = (existingStyle == selectedStyle) ? -1 : selectedStyle;
    processor.setSequencerCell (dragRow, col, dragTargetStyle);
    repaint();
}

void SequencerGrid::mouseDrag (const juce::MouseEvent& event)
{
    if (extendRow >= 0)
    {
        const int numColumns = processor.getSequencerNumSteps();
        const int columnWidth = getColumnWidth();
        const int naturalSteps = processor.getSequencerNaturalLengthSteps (extendRow);

        // Growth-only for this pass (per its own spec) -- dragging left of
        // the step's own natural length just holds it there rather than
        // shrinking below it.
        const int rawColumn = event.x / columnWidth;
        const int endColumnInclusive = juce::jlimit (extendStartColumn, numColumns - 1, rawColumn);
        const int draggedLength = endColumnInclusive - extendStartColumn + 1;
        extendLiveLengthSteps = juce::jmax (naturalSteps, draggedLength);

        repaint();
        return;
    }

    if (sliderDragging)
    {
        const int numRows = processor.getSequencerNumRows();
        const int numColumns = processor.getSequencerNumSteps();
        const auto sliderBounds = getParameterSliderBounds (editingRow, editingColumn, numRows, numColumns);
        updateEditingValueFromMouseX (event.x, sliderBounds);
        repaint();
        return;
    }

    if (dragRow < 0)
        return;

    // Row is locked to wherever the drag started -- "click-drag across
    // cells in A row," not a free paint across the whole grid.
    const int col = getColumnIndexAtX (event.x);
    processor.setSequencerCell (dragRow, col, dragTargetStyle);
    repaint();
}

void SequencerGrid::mouseUp (const juce::MouseEvent&)
{
    if (extendRow >= 0)
    {
        // Release commits (Pass 1) -- the whole gesture up to now was a
        // local, uncommitted preview only; this is the one point it
        // actually reaches the processor (and, in turn, clears any other
        // row's conflicting cells the newly-claimed span now covers).
        processor.setSequencerCellExtendedLengthSteps (extendRow, extendStartColumn, extendLiveLengthSteps);
        extendRow = -1;
        extendStartColumn = -1;
        repaint();
        return;
    }

    if (sliderDragging)
    {
        sliderDragging = false;
        editingRow = -1; // Step 45 -- slider closes automatically on release
        repaint();
    }

    dragRow = -1;
}
