#include "WaveformDisplay.h"
#include <cmath>

WaveformDisplay::WaveformDisplay (SlicerAudioProcessor& processorToUse)
    : processor (processorToUse)
{
    startTimerHz (30); // polls the currently-playing slice for the highlight
}

void WaveformDisplay::timerCallback()
{
    if (! processor.hasSample())
        return;

    const auto mousePos = getMouseXYRelative();
    const bool hovering = getLocalBounds().contains (mousePos);
    const auto mods = juce::ModifierKeys::currentModifiers;

    hoveredDeletableSamplePosition = -1;
    hoveredFreePlaceSamplePosition = -1;

    if (hovering && ! hasPreview)
    {
        if (mods.isCommandDown())
        {
            const int manualId = findManualPointNear (mousePos.x);

            if (manualId >= 0)
            {
                for (const auto& mp : processor.getManualSlicePoints())
                    if (mp.id == manualId)
                        hoveredDeletableSamplePosition = mp.samplePosition;
            }
            else
            {
                hoveredDeletableSamplePosition = findAutoPointNear (mousePos.x);
            }
        }
        else if (mods.isShiftDown())
        {
            const int manualId = findManualPointNear (mousePos.x);

            if (manualId >= 0)
                for (const auto& mp : processor.getManualSlicePoints())
                    if (mp.id == manualId)
                        hoveredFreePlaceSamplePosition = mp.samplePosition;
        }
    }

    repaint();
}

void WaveformDisplay::showPreviewSlices (const std::vector<Slice>& preview)
{
    previewSlices = preview;
    hasPreview = true;
    repaint();
}

void WaveformDisplay::clearPreviewSlices()
{
    hasPreview = false;
    repaint();
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (bounds);

    if (isDraggingOver)
    {
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.fillRect (bounds);
    }

    if (! processor.hasSample())
    {
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (14.0f);
        g.drawFittedText ("Drag and drop a sample here, or use Load Sample above",
                           getLocalBounds(), juce::Justification::centred, 2);
        return;
    }

    // --- Waveform itself ---
    const float midY = bounds.getCentreY();
    const float halfHeight = bounds.getHeight() * 0.45f;

    g.setColour (juce::Colours::cyan.withAlpha (0.8f));

    for (int x = 0; x < (int) waveformPeaks.size(); ++x)
    {
        const auto& peak = waveformPeaks[(size_t) x];
        const float y1 = midY - peak.second * halfHeight;
        const float y2 = midY - peak.first * halfHeight;
        g.drawVerticalLine (x, y1, y2);
    }

    // --- Trim markers (Step 23): dim everything outside [trimStart, trimEnd)
    // and draw two draggable flagged handles, distinct in colour/shape
    // from slice/manual/auto boundary lines so they're never confused. ---
    {
        const int totalSamplesForTrim = processor.getSampleBuffer().getNumSamples();

        if (totalSamplesForTrim > 0)
        {
            const float trimStartX = sampleToX (processor.getTrimStartSample());
            const float trimEndX = sampleToX (processor.getTrimEndSample());

            g.setColour (juce::Colours::black.withAlpha (0.6f));

            if (trimStartX > bounds.getX())
                g.fillRect (bounds.getX(), bounds.getY(), trimStartX - bounds.getX(), bounds.getHeight());

            if (trimEndX < bounds.getRight())
                g.fillRect (trimEndX, bounds.getY(), bounds.getRight() - trimEndX, bounds.getHeight());

            g.setColour (juce::Colours::yellow.withAlpha (0.9f));
            g.drawVerticalLine ((int) trimStartX, bounds.getY(), bounds.getBottom());
            g.drawVerticalLine ((int) trimEndX, bounds.getY(), bounds.getBottom());

            constexpr float flagSize = 9.0f;
            juce::Path startFlag, endFlag;
            startFlag.addTriangle (trimStartX, bounds.getY(), trimStartX + flagSize, bounds.getY(), trimStartX, bounds.getY() + flagSize);
            endFlag.addTriangle (trimEndX, bounds.getY(), trimEndX - flagSize, bounds.getY(), trimEndX, bounds.getY() + flagSize);
            g.fillPath (startFlag);
            g.fillPath (endFlag);
        }
    }

    // --- Audition playhead (Step 28): a thin, distinctly-coloured
    // vertical line at the audition engine's current read position.
    // Independent of slicing entirely (drawn here, before the per-slice
    // loop's own early-return below, so it still shows even with zero
    // slices) — Audition bypasses slicing completely, same as it bypasses
    // the rest of the generative engine. Mutually exclusive with the
    // generative playhead highlight further down: Audition and the
    // generative engine can never run at the same time (Audition
    // auto-stops the instant host transport starts), so no overlap logic
    // is needed here — this is simply never true at the same moment
    // getCurrentlyPlayingSliceIndex() would be. ---
    {
        const int auditionPosition = processor.getAuditionPlaybackPosition();
        const int totalSamplesForAudition = processor.getSampleBuffer().getNumSamples();

        if (auditionPosition >= 0 && totalSamplesForAudition > 0
            && auditionPosition >= visibleStartSample && auditionPosition < visibleEndSample)
        {
            const float auditionX = sampleToX (auditionPosition);

            g.setColour (juce::Colours::dodgerblue);
            g.drawVerticalLine ((int) auditionX, bounds.getY(), bounds.getBottom());
        }
    }

    // --- Beat-number grid (Step 31): informational only -- a small tick
    // + number (1, 2, 3...) at each of loopLengthBars*4 evenly-spaced
    // positions across [trimStart, trimEnd), the existing 4/4 assumption
    // used elsewhere in the engine. Deliberately small/muted so it reads
    // as a reference grid, not competing visually with the slice
    // boundaries/faders/trim handles/playheads drawn elsewhere. Only
    // drawn for beats that fall within the CURRENT VISIBLE range -- a
    // beat scrolled out of view is simply skipped, not drawn off-screen
    // and relying on clipping, since there could be many of them (up to
    // loopLengthBars*4) and most would be outside the view at any
    // reasonable zoom level. ---
    {
        const int totalBeats = processor.getLoopLengthBars() * 4;
        const int trimStartForGrid = processor.getTrimStartSample();
        const int trimEndForGrid = processor.getTrimEndSample();
        const int trimSpanForGrid = trimEndForGrid - trimStartForGrid;

        if (totalBeats > 0 && trimSpanForGrid > 0)
        {
            g.setColour (juce::Colours::lightgrey.withAlpha (0.55f));
            g.setFont (10.0f);

            for (int i = 0; i < totalBeats; ++i)
            {
                const int beatSample = trimStartForGrid + (int) (((juce::int64) i * trimSpanForGrid) / totalBeats);

                if (beatSample < visibleStartSample || beatSample >= visibleEndSample)
                    continue; // scrolled out of view

                const float beatX = sampleToX (beatSample);

                g.drawVerticalLine ((int) beatX, bounds.getY(), bounds.getY() + 6.0f);
                g.drawText (juce::String (i + 1), (int) beatX + 2, (int) bounds.getY(), 24, 12, juce::Justification::left);
            }
        }
    }

    // --- Slice boundaries + probability faders ---
    // While a live preview is active (e.g. dragging the sensitivity
    // slider), draw the proposed layout instead of the committed one.
    // Indices in a preview don't necessarily match committed slice
    // indices, so the probability fader and playhead highlight — both of
    // which are keyed by index — are suppressed during a preview.
    const auto slices = hasPreview ? previewSlices : processor.getSlices();
    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0 || slices.empty())
        return;

    const auto manualPoints = processor.getManualSlicePoints();
    const int playingIndex = hasPreview ? -1 : processor.getCurrentlyPlayingSliceIndex();

    auto isManualBoundary = [&manualPoints] (int samplePos)
    {
        for (const auto& mp : manualPoints)
            if (mp.samplePosition == samplePos)
                return true;

        return false;
    };

    for (size_t i = 0; i < slices.size(); ++i)
    {
        const float startX = sampleToX (slices[i].startSample);
        const float endX = sampleToX (slices[i].endSample);

        // Playhead highlight: a bright wash behind everything else for
        // whichever slice is currently sounding — drawn first so the
        // boundary line and fader stay legible on top of it.
        if ((int) i == playingIndex)
        {
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.fillRect (startX, bounds.getY(), juce::jmax (1.0f, endX - startX), bounds.getHeight());
        }

        // Cmd-hover cue: a red wash behind whichever boundary the user is
        // currently hovering with Cmd held, making it clear which point
        // will be deleted before they actually click.
        if (! hasPreview && slices[i].startSample == hoveredDeletableSamplePosition)
        {
            g.setColour (juce::Colours::red.withAlpha (0.35f));
            g.fillRect (startX - 5.0f, bounds.getY(), 10.0f, bounds.getHeight());
        }

        // Shift-hover cue: a distinct green wash — dragging from here
        // would bypass snapping entirely (free placement).
        if (! hasPreview && slices[i].startSample == hoveredFreePlaceSamplePosition)
        {
            g.setColour (juce::Colours::limegreen.withAlpha (0.35f));
            g.fillRect (startX - 5.0f, bounds.getY(), 10.0f, bounds.getHeight());
        }

        const bool manual = isManualBoundary (slices[i].startSample);

        if (manual)
        {
            // Vivid, unmistakably-different colour + extra width so manual
            // points are never confused with auto-detected ones at a glance.
            g.setColour (juce::Colours::magenta.withAlpha (0.95f));
            g.fillRect (startX - 1.0f, bounds.getY(), 3.0f, bounds.getHeight());
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawVerticalLine ((int) startX, bounds.getY(), bounds.getBottom());
        }

        if (hasPreview)
            continue; // don't draw a fader for a proposed-but-uncommitted layout

        // Probability fader: a translucent bar spanning this slice's full
        // width. Height is proportional to weight — empty at the bottom
        // (never picked), full height at the top (always picked, relative
        // to the other slices' weights).
        const float probability = processor.getSliceProbability ((int) i);
        const float faderWidth = juce::jmax (1.0f, endX - startX);
        const float faderX = startX;
        const float faderHeight = bounds.getHeight() * probability;
        const float faderY = bounds.getBottom() - faderHeight;

        g.setColour (juce::Colours::orange.withAlpha (0.5f));
        g.fillRect (faderX, faderY, faderWidth, faderHeight);

        g.setColour (juce::Colours::orange.withAlpha (0.9f));
        g.drawRect (faderX, bounds.getY(), faderWidth, bounds.getHeight(), 1.0f);
    }
}

void WaveformDisplay::resized()
{
    rebuildWaveformPeaks(); // peak columns depend on the component's pixel width
}

void WaveformDisplay::rebuildWaveformPeaks()
{
    waveformPeaks.clear();

    if (! processor.hasSample())
        return;

    const auto& buffer = processor.getSampleBuffer();
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const int width = juce::jmax (1, getWidth());

    if (numSamples == 0 || numChannels == 0)
        return;

    waveformPeaks.resize ((size_t) width);

    // Step 31: peaks are computed over the current visible range mapped to
    // the component's pixel width, not the whole buffer -- each column's
    // [startSample, endSample) is a proportional slice of that range
    // (float-based, not a fixed samplesPerPixel step) so this stays
    // correct whether zoomed out (many samples per pixel) or zoomed in
    // (fewer samples than pixels, in which case adjacent columns can
    // legitimately share/repeat samples -- acceptable given minVisible-
    // RangeSamples() already stops zoom well short of single-sample
    // territory).
    const int visibleRange = juce::jmax (1, visibleEndSample - visibleStartSample);

    for (int x = 0; x < width; ++x)
    {
        const int rawStart = visibleStartSample + (int) (((juce::int64) x * visibleRange) / width);
        const int rawEnd = visibleStartSample + (int) (((juce::int64) (x + 1) * visibleRange) / width);
        const int startSample = juce::jlimit (0, numSamples, rawStart);
        const int endSample = (startSample >= numSamples) ? startSample
                                                            : juce::jlimit (startSample + 1, numSamples, rawEnd);

        float minVal = 0.0f;
        float maxVal = 0.0f;

        for (int s = startSample; s < endSample; ++s)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float sample = buffer.getSample (ch, s);
                minVal = juce::jmin (minVal, sample);
                maxVal = juce::jmax (maxVal, sample);
            }
        }

        waveformPeaks[(size_t) x] = { minVal, maxVal };
    }
}

int WaveformDisplay::getSliceIndexAtX (int x) const
{
    if (! processor.hasSample())
        return -1;

    const auto& slices = processor.getSlices();
    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0 || slices.empty())
        return -1;

    const int samplePos = xToSample (x);

    for (size_t i = 0; i < slices.size(); ++i)
        if (samplePos >= slices[i].startSample && samplePos < slices[i].endSample)
            return (int) i;

    return (int) slices.size() - 1; // right at the very end — count it as the last slice
}

int WaveformDisplay::xToSample (int x) const
{
    // Step 31: maps through the current visible range, not the whole
    // buffer -- the one seam every mouse-position caller in this class
    // already goes through, so panning/zooming doesn't require touching
    // any of them individually.
    const int visibleRange = visibleEndSample - visibleStartSample;
    const float width = (float) juce::jmax (1, getWidth());
    const float fraction = juce::jlimit (0.0f, 1.0f, (float) x / width);
    return visibleStartSample + (int) (fraction * (float) visibleRange);
}

float WaveformDisplay::sampleToX (int sample) const
{
    // Inverse of xToSample() -- also maps through the visible range.
    // Deliberately NOT clamped to [0, width): a sample outside the
    // current view legitimately maps to a negative or over-width pixel
    // position, and callers either skip drawing it (see the beat grid and
    // audition playhead in paint()) or rely on JUCE's own clipping to
    // discard the harmless off-screen draw (trim handles, slice markers).
    const int visibleRange = juce::jmax (1, visibleEndSample - visibleStartSample);
    const float width = (float) juce::jmax (1, getWidth());
    return ((float) (sample - visibleStartSample) / (float) visibleRange) * width;
}

int WaveformDisplay::findManualPointNear (int x) const
{
    if (! processor.hasSample())
        return -1;

    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0)
        return -1;

    constexpr float hitRadiusPixels = 6.0f;

    for (const auto& mp : processor.getManualSlicePoints())
    {
        const float px = sampleToX (mp.samplePosition);

        if (std::abs (px - (float) x) <= hitRadiusPixels)
            return mp.id;
    }

    return -1;
}

int WaveformDisplay::findAutoPointNear (int x) const
{
    if (! processor.hasSample())
        return -1;

    const auto& slices = processor.getSlices();
    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0 || slices.empty())
        return -1;

    constexpr float hitRadiusPixels = 6.0f;

    for (const auto& slice : slices)
    {
        if (slice.startSample == 0)
            continue; // never excludable

        const float px = sampleToX (slice.startSample);

        if (std::abs (px - (float) x) <= hitRadiusPixels)
            return slice.startSample; // a sample position, not an id — auto points don't have ids
    }

    return -1;
}

WaveformDisplay::TrimHandle WaveformDisplay::findTrimHandleNear (int x) const
{
    if (! processor.hasSample())
        return TrimHandle::none;

    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0)
        return TrimHandle::none;

    constexpr float hitRadiusPixels = 8.0f;

    const float startX = sampleToX (processor.getTrimStartSample());
    const float endX = sampleToX (processor.getTrimEndSample());

    if (std::abs (startX - (float) x) <= hitRadiusPixels)
        return TrimHandle::start;

    if (std::abs (endX - (float) x) <= hitRadiusPixels)
        return TrimHandle::end;

    return TrimHandle::none;
}

void WaveformDisplay::setProbabilityFromMouse (const juce::MouseEvent& event)
{
    const int sliceIndex = getSliceIndexAtX (event.x);

    if (sliceIndex < 0)
        return;

    const float probability = 1.0f - juce::jlimit (0.0f, 1.0f, (float) event.y / (float) juce::jmax (1, getHeight()));
    processor.setSliceProbability (sliceIndex, probability);
    repaint();
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& event)
{
    draggingManualPointId = -1;
    dragStartSamplePosition = -1;
    draggingTrimHandle = TrimHandle::none;

    if (! processor.hasSample())
        return;

    const auto trimHandle = findTrimHandleNear (event.x);

    if (trimHandle != TrimHandle::none)
    {
        draggingTrimHandle = trimHandle;
        return;
    }

    const int nearManualId = findManualPointNear (event.x);

    if (nearManualId >= 0)
    {
        if (event.mods.isCommandDown() || event.getNumberOfClicks() >= 2)
        {
            processor.removeManualSlicePoint (nearManualId);
            refresh();
            return;
        }

        draggingManualPointId = nearManualId; // subsequent drag moves this point, not a fader

        for (const auto& mp : processor.getManualSlicePoints())
            if (mp.id == nearManualId)
                dragStartSamplePosition = mp.samplePosition;

        return;
    }

    const int nearAutoSample = findAutoPointNear (event.x);

    if (nearAutoSample >= 0 && (event.mods.isCommandDown() || event.getNumberOfClicks() >= 2))
    {
        processor.excludeNearestAutoPoint (nearAutoSample);
        refresh();
        return;
    }

    if (event.getNumberOfClicks() >= 2)
    {
        const bool snap = ! event.mods.isShiftDown();
        processor.addManualSlicePoint (xToSample (event.x), snap);
        refresh();
        return;
    }

    setProbabilityFromMouse (event);
}

void WaveformDisplay::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingTrimHandle == TrimHandle::start)
    {
        autoPanIfNearEdge (event.x);
        const int beforeTrimStart = processor.getTrimStartSample();
        const bool snap = ! event.mods.isShiftDown();
        processor.setTrimStartSample (xToSample (event.x), snap);
        refresh();

        // Only fire on an ACTUAL change (Step 33) -- a drag that's
        // clamped at the same position every frame (e.g. pinned against
        // the other handle, or against the buffer's own edge) shouldn't
        // keep re-flagging Loop Length as stale for a value that never
        // moved.
        if (onTrimChanged && processor.getTrimStartSample() != beforeTrimStart)
            onTrimChanged();

        return;
    }

    if (draggingTrimHandle == TrimHandle::end)
    {
        autoPanIfNearEdge (event.x);
        const int beforeTrimEnd = processor.getTrimEndSample();
        const bool snap = ! event.mods.isShiftDown();
        processor.setTrimEndSample (xToSample (event.x), snap);
        refresh();

        if (onTrimChanged && processor.getTrimEndSample() != beforeTrimEnd)
            onTrimChanged();

        return;
    }

    if (draggingManualPointId >= 0)
    {
        autoPanIfNearEdge (event.x);
        const bool snap = ! event.mods.isShiftDown();
        processor.moveManualSlicePoint (draggingManualPointId, xToSample (event.x), snap);
        refresh();
        return;
    }

    setProbabilityFromMouse (event);
}

void WaveformDisplay::mouseUp (const juce::MouseEvent&)
{
    if (draggingManualPointId >= 0 && dragStartSamplePosition >= 0)
        processor.commitManualPointMove (draggingManualPointId, dragStartSamplePosition);

    draggingManualPointId = -1;
    dragStartSamplePosition = -1;
    draggingTrimHandle = TrimHandle::none;
}

void WaveformDisplay::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! processor.hasSample())
        return;

    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0)
        return;

    // Trackpad "shift the vertical gesture into deltaX" quirks aside, the
    // dominant axis is whichever one actually moved -- deltaY normally,
    // deltaX as a fallback (some trackpads report a pure-horizontal
    // gesture there instead, e.g. a two-finger swipe with Shift held).
    const float delta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;

    if (delta == 0.0f)
        return;

    const int visibleRange = visibleEndSample - visibleStartSample;

    if (event.mods.isShiftDown())
    {
        // Pan: shift both bounds by the same amount -- a fraction of the
        // current visible range, so one notch feels proportionally
        // similar whether zoomed in or fully out.
        const int panSamples = (int) (delta * (float) visibleRange * panFractionPerNotch);

        int newStart = visibleStartSample - panSamples;
        int newEnd = visibleEndSample - panSamples;

        if (newStart < 0)
        {
            newEnd -= newStart;
            newStart = 0;
        }

        if (newEnd > totalSamples)
        {
            newStart -= (newEnd - totalSamples);
            newEnd = totalSamples;
        }

        visibleStartSample = juce::jmax (0, newStart);
        visibleEndSample = juce::jmin (totalSamples, newEnd);
    }
    else
    {
        // Zoom: adjust the range symmetrically around the sample under the
        // cursor, so whatever's under the mouse stays put as the range
        // narrows/widens -- standard "zoom to cursor" behaviour.
        const int sampleUnderCursor = xToSample (event.x);
        const float zoomFactor = std::pow (zoomFactorPerNotch, -delta); // < 1 shrinks the range (zoom in)

        const double leftFraction = visibleRange > 0
            ? juce::jlimit (0.0, 1.0, (double) (sampleUnderCursor - visibleStartSample) / (double) visibleRange)
            : 0.5;

        const int newRange = juce::jmax (minVisibleRangeSamples(),
                                          (int) ((double) visibleRange * (double) zoomFactor));

        int newStart = sampleUnderCursor - (int) (leftFraction * (double) newRange);
        int newEnd = newStart + newRange;

        if (newStart < 0)
        {
            newEnd -= newStart;
            newStart = 0;
        }

        if (newEnd > totalSamples)
        {
            newStart -= (newEnd - totalSamples);
            newEnd = totalSamples;
        }

        visibleStartSample = juce::jmax (0, newStart);
        visibleEndSample = juce::jmin (totalSamples, newEnd);
    }

    clampVisibleRange();
    rebuildWaveformPeaks();
    repaint();
}

void WaveformDisplay::zoomToTrims()
{
    if (! processor.hasSample())
        return;

    const int totalSamples = processor.getSampleBuffer().getNumSamples();
    const int trimStart = processor.getTrimStartSample();
    const int trimEnd = processor.getTrimEndSample();
    const int trimSpan = trimEnd - trimStart;

    if (trimSpan <= 0)
        return;

    // A small margin on each side so the trim handles themselves stay
    // grabbable even when they sit right at the trim boundary -- a handle
    // drawn exactly at the edge of the visible range is awkward to grab.
    const int margin = juce::jmax (minVisibleRangeSamples() / 4, trimSpan / 20);

    visibleStartSample = juce::jmax (0, trimStart - margin);
    visibleEndSample = juce::jmin (totalSamples, trimEnd + margin);

    clampVisibleRange();
    rebuildWaveformPeaks();
    repaint();
}

void WaveformDisplay::resetZoom()
{
    if (! processor.hasSample())
        return;

    visibleStartSample = 0;
    visibleEndSample = processor.getSampleBuffer().getNumSamples();

    rebuildWaveformPeaks();
    repaint();
}

int WaveformDisplay::minVisibleRangeSamples() const
{
    const double sr = processor.hasSample() ? processor.getSampleSampleRate() : 44100.0;
    return juce::jmax (32, (int) (minVisibleRangeMs / 1000.0 * sr));
}

void WaveformDisplay::clampVisibleRange()
{
    const int totalSamples = processor.hasSample() ? processor.getSampleBuffer().getNumSamples() : 0;

    visibleStartSample = juce::jlimit (0, totalSamples, visibleStartSample);
    visibleEndSample = juce::jlimit (visibleStartSample, totalSamples, visibleEndSample);

    const int minRange = minVisibleRangeSamples();

    if (visibleEndSample - visibleStartSample < minRange)
    {
        visibleEndSample = juce::jmin (totalSamples, visibleStartSample + minRange);
        visibleStartSample = juce::jmax (0, visibleEndSample - minRange);
    }
}

void WaveformDisplay::autoPanIfNearEdge (int x)
{
    if (! processor.hasSample())
        return;

    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0)
        return;

    const int width = juce::jmax (1, getWidth());
    const int visibleRange = visibleEndSample - visibleStartSample;

    int panSamples = 0;

    if (x < autoPanEdgeThresholdPixels)
        panSamples = -(int) ((float) visibleRange * autoPanFractionPerDragEvent);
    else if (x > width - autoPanEdgeThresholdPixels)
        panSamples = (int) ((float) visibleRange * autoPanFractionPerDragEvent);

    if (panSamples == 0)
        return;

    // Same clamp-to-[0, totalSamples] shape as mouseWheelMove()'s pan
    // branch -- range width is preserved, just shifted.
    int newStart = visibleStartSample + panSamples;
    int newEnd = visibleEndSample + panSamples;

    if (newStart < 0)
    {
        newEnd -= newStart;
        newStart = 0;
    }

    if (newEnd > totalSamples)
    {
        newStart -= (newEnd - totalSamples);
        newEnd = totalSamples;
    }

    visibleStartSample = juce::jmax (0, newStart);
    visibleEndSample = juce::jmin (totalSamples, newEnd);

    // Deliberately no rebuildWaveformPeaks()/repaint() here -- every
    // caller already follows this with refresh() (which does both) once
    // it's finished computing this drag event's new position from the
    // now-updated visible range.
}

bool WaveformDisplay::isSupportedAudioFile (const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}

bool WaveformDisplay::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isSupportedAudioFile (juce::File (f)))
            return true;

    return false;
}

void WaveformDisplay::fileDragEnter (const juce::StringArray&, int, int)
{
    isDraggingOver = true;
    repaint();
}

void WaveformDisplay::fileDragExit (const juce::StringArray&)
{
    isDraggingOver = false;
    repaint();
}

void WaveformDisplay::filesDropped (const juce::StringArray& files, int /*x*/, int /*y*/)
{
    isDraggingOver = false;

    for (const auto& f : files)
    {
        const juce::File file (f);

        if (isSupportedAudioFile (file))
        {
            processor.loadSample (file);
            refresh();

            if (onSampleChanged)
                onSampleChanged();

            break;
        }
    }

    repaint();
}

void WaveformDisplay::refresh()
{
    hasPreview = false; // a real refresh means there's now committed data to show

    const int totalSamples = processor.hasSample() ? processor.getSampleBuffer().getNumSamples() : 0;

    if (totalSamples != lastKnownTotalSamples)
    {
        // A genuinely new sample loaded (different length) -- any previous
        // zoom/pan view belonged to the old buffer and is now meaningless,
        // so reset back to fully zoomed out. See lastKnownTotalSamples'
        // doc comment for why buffer length is what's used to detect this
        // rather than every refresh() call resetting the view (refresh()
        // is also called on every re-slice/trim-drag/manual edit, none of
        // which should touch zoom).
        visibleStartSample = 0;
        visibleEndSample = totalSamples;
        lastKnownTotalSamples = totalSamples;
    }
    else
    {
        clampVisibleRange();
    }

    rebuildWaveformPeaks();
    repaint();
}
