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
            const float trimStartX = ((float) processor.getTrimStartSample() / (float) totalSamplesForTrim) * bounds.getWidth();
            const float trimEndX = ((float) processor.getTrimEndSample() / (float) totalSamplesForTrim) * bounds.getWidth();

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

    const float width = bounds.getWidth();
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
        const float startX = ((float) slices[i].startSample / (float) totalSamples) * width;
        const float endX = ((float) slices[i].endSample / (float) totalSamples) * width;

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
    const int samplesPerPixel = juce::jmax (1, numSamples / width);

    for (int x = 0; x < width; ++x)
    {
        const int startSample = x * samplesPerPixel;
        const int endSample = juce::jmin (numSamples, startSample + samplesPerPixel);

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

    const float width = (float) juce::jmax (1, getWidth());
    const float fraction = juce::jlimit (0.0f, 1.0f, (float) x / width);
    const int samplePos = (int) (fraction * (float) totalSamples);

    for (size_t i = 0; i < slices.size(); ++i)
        if (samplePos >= slices[i].startSample && samplePos < slices[i].endSample)
            return (int) i;

    return (int) slices.size() - 1; // right at the very end — count it as the last slice
}

int WaveformDisplay::xToSample (int x) const
{
    const int totalSamples = processor.getSampleBuffer().getNumSamples();
    const float width = (float) juce::jmax (1, getWidth());
    const float fraction = juce::jlimit (0.0f, 1.0f, (float) x / width);
    return (int) (fraction * (float) totalSamples);
}

int WaveformDisplay::findManualPointNear (int x) const
{
    if (! processor.hasSample())
        return -1;

    const int totalSamples = processor.getSampleBuffer().getNumSamples();

    if (totalSamples <= 0)
        return -1;

    const float width = (float) juce::jmax (1, getWidth());
    constexpr float hitRadiusPixels = 6.0f;

    for (const auto& mp : processor.getManualSlicePoints())
    {
        const float px = ((float) mp.samplePosition / (float) totalSamples) * width;

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

    const float width = (float) juce::jmax (1, getWidth());
    constexpr float hitRadiusPixels = 6.0f;

    for (const auto& slice : slices)
    {
        if (slice.startSample == 0)
            continue; // never excludable

        const float px = ((float) slice.startSample / (float) totalSamples) * width;

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

    const float width = (float) juce::jmax (1, getWidth());
    constexpr float hitRadiusPixels = 8.0f;

    const float startX = ((float) processor.getTrimStartSample() / (float) totalSamples) * width;
    const float endX = ((float) processor.getTrimEndSample() / (float) totalSamples) * width;

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
        processor.setTrimStartSample (xToSample (event.x));
        refresh();

        if (onTrimChanged)
            onTrimChanged();

        return;
    }

    if (draggingTrimHandle == TrimHandle::end)
    {
        processor.setTrimEndSample (xToSample (event.x));
        refresh();

        if (onTrimChanged)
            onTrimChanged();

        return;
    }

    if (draggingManualPointId >= 0)
    {
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
    rebuildWaveformPeaks();
    repaint();
}
