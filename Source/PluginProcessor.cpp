#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <array>

namespace
{
    // Snapshot-based undo action (Step 12): every slice-editing operation
    // (add/move/remove/exclude/reset) is captured as "manual+excluded
    // point state before" vs "...after", and undo/redo just re-applies
    // whichever snapshot is needed. One class covers all of them rather
    // than a bespoke action type per operation — coarse-grained on
    // purpose, keeps the system simple and uniform.
    class SliceEditUndoableAction : public juce::UndoableAction
    {
    public:
        SliceEditUndoableAction (SlicerAudioProcessor& processorToUse,
                                  std::vector<SlicerAudioProcessor::ManualPointInfo> beforeManual,
                                  std::vector<SlicerAudioProcessor::ManualPointInfo> beforeExcluded,
                                  std::vector<SlicerAudioProcessor::ManualPointInfo> afterManual,
                                  std::vector<SlicerAudioProcessor::ManualPointInfo> afterExcluded)
            : processor (processorToUse),
              before { std::move (beforeManual), std::move (beforeExcluded) },
              after { std::move (afterManual), std::move (afterExcluded) }
        {
        }

        bool perform() override
        {
            processor.applyManualState (after.first, after.second);
            return true;
        }

        bool undo() override
        {
            processor.applyManualState (before.first, before.second);
            return true;
        }

    private:
        SlicerAudioProcessor& processor;
        std::pair<std::vector<SlicerAudioProcessor::ManualPointInfo>,
                  std::vector<SlicerAudioProcessor::ManualPointInfo>> before, after;
    };

    // Note-value palette shared by the clock-reference menu and the
    // subdivision probability table (Step 14/15). Matches the standard
    // Max/M4L tempo-relative rate set, sorted shortest to longest, capped
    // at 1n (1nd — 1.5 bars — deliberately excluded per the "no longer
    // than 1 bar" decision). Beats are quarter-note units.
    struct NoteValueOption { const char* name; double beats; };

    const std::array<NoteValueOption, SlicerAudioProcessor::numNoteValueOptions> noteValueOptions { {
        { "128n", 1.0 / 32.0 },
        { "64n",  1.0 / 16.0 },
        { "32nt", 1.0 / 12.0 },
        { "64nd", 3.0 / 32.0 },
        { "32n",  1.0 / 8.0 },
        { "16nt", 1.0 / 6.0 },
        { "32nd", 3.0 / 16.0 },
        { "16n",  1.0 / 4.0 },
        { "8nt",  1.0 / 3.0 },
        { "16nd", 3.0 / 8.0 },
        { "8n",   1.0 / 2.0 },
        { "4nt",  2.0 / 3.0 },
        { "8nd",  3.0 / 4.0 },
        { "4n",   1.0 },
        { "2nt",  4.0 / 3.0 },
        { "4nd",  3.0 / 2.0 },
        { "2n",   2.0 },
        { "1nt",  8.0 / 3.0 },
        { "2nd",  3.0 },
        { "1n",   4.0 }
    } };
}

juce::String SlicerAudioProcessor::getNoteValueName (int index)
{
    if (index < 0 || index >= numNoteValueOptions)
        return {};

    return noteValueOptions[(size_t) index].name;
}

double SlicerAudioProcessor::getNoteValueBeats (int index)
{
    if (index < 0 || index >= numNoteValueOptions)
        return 1.0;

    return noteValueOptions[(size_t) index].beats;
}

//==============================================================================
SlicerAudioProcessor::SlicerAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();
    subdivisionProbabilities.assign (numNoteValueOptions, 1.0f);
}

SlicerAudioProcessor::~SlicerAudioProcessor() = default;

void SlicerAudioProcessor::prepareToPlay (double /*sampleRate*/, int /*samplesPerBlock*/)
{
    hasCurrentPick = false;
    clockModeInitialized = false;
    clockCurrentPickValid = false;
}

void SlicerAudioProcessor::releaseResources() {}

bool SlicerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SlicerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    midiMessages.clear(); // transport-driven now — no MIDI in or out
    buffer.clear();

    const juce::ScopedLock sl (sampleLock);

    if (! sampleLoaded || slices.empty())
        return;

    auto* playHead = getPlayHead();

    if (playHead == nullptr)
        return;

    const auto position = playHead->getPosition();

    if (! position.hasValue() || ! position->getIsPlaying())
    {
        hasCurrentPick = false; // transport stopped — fresh chain next time it starts
        clockModeInitialized = false;
        clockCurrentPickValid = false;
        currentlyPlayingSliceIndexForUI.store (-1);
        return;
    }

    const double hostBpm = position->getBpm().hasValue() ? *position->getBpm() : 120.0;
    const double hostSampleRate = getSampleRate();

    if (hostBpm <= 0.0 || hostSampleRate <= 0.0)
        return;

    // Repitch factor: how much faster/slower to play the source sample so
    // its `loopLengthBars` bars match the host's tempo. >1 = source is
    // slower than host (speed up to fit, pitch rises); <1 = source is
    // faster than host (slow down, pitch drops). Applies in both trigger
    // modes — it's purely a playback-speed/pitch thing, independent of
    // when triggers happen.
    const double loopLengthQuarterNotes = (double) loopLengthBars.load() * 4.0; // assumes 4/4
    const double sourceLoopLengthSeconds = (double) sampleBuffer.getNumSamples() / sampleSampleRate;
    const double hostLoopLengthSeconds = loopLengthQuarterNotes * (60.0 / hostBpm);
    const double repitchRatio = (hostLoopLengthSeconds > 0.0)
                                     ? (sourceLoopLengthSeconds / hostLoopLengthSeconds)
                                     : 1.0;
    const double playbackRate = (sampleSampleRate / hostSampleRate) * repitchRatio;

    const int sourceLength = sampleBuffer.getNumSamples();
    const int sourceChannels = sampleBuffer.getNumChannels();
    const int outChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (sourceLength == 0 || sourceChannels == 0)
        return;

    const double fadeInSamplesRequested = (double) fadeInMs.load() / 1000.0 * hostSampleRate;
    const double fadeOutSamplesRequested = (double) fadeOutMs.load() / 1000.0 * hostSampleRate;

    const bool clockMode = (triggerMode.load() == TriggerMode::clock);

    // Only Clock mode needs the host's beat position — Slice Length mode
    // paces itself purely from slice content length and never looks at ppq.
    double ppqStart = 0.0;
    double ppqPerSample = 0.0;

    if (clockMode)
    {
        ppqStart = position->getPpqPosition().hasValue() ? *position->getPpqPosition() : 0.0;
        ppqPerSample = (hostBpm / 60.0) / hostSampleRate;

        if (! clockModeInitialized)
        {
            // Just entered Clock mode, or transport just started — snap to
            // the window we're currently inside and force an immediate
            // pick on the very first sample below.
            const double windowBeats = getNoteValueBeats (clockReferenceIndex.load());
            const juce::int64 windowIndex = (juce::int64) std::floor (ppqStart / windowBeats);
            windowEndPpq = (double) (windowIndex + 1) * windowBeats;
            nextTickPpq = ppqStart;
            clockModeInitialized = true;
        }
    }
    else
    {
        clockModeInitialized = false; // so re-entering Clock mode later starts fresh
    }

    for (int i = 0; i < numSamples; ++i)
    {
        if (clockMode)
        {
            const double samplePpq = ppqStart + (double) i * ppqPerSample;

            if (samplePpq >= nextTickPpq)
            {
                const bool newWindow = ! clockCurrentPickValid || samplePpq >= windowEndPpq;

                if (newWindow)
                {
                    clockCurrentSliceIndex = pickWeightedRandomSlice();
                    clockCurrentSubdivisionIndex = pickWeightedIndex (subdivisionProbabilities);
                    clockCurrentPickValid = true;

                    const double windowBeats = getNoteValueBeats (clockReferenceIndex.load());
                    const juce::int64 windowIndex = (juce::int64) std::floor (samplePpq / windowBeats);
                    windowEndPpq = (double) (windowIndex + 1) * windowBeats;
                }

                // Retrigger (or first-trigger) this window's slice from its
                // start — unconditionally, even if it hadn't naturally
                // finished yet. That's the whole point of Clock mode.
                if (clockCurrentSliceIndex >= 0 && clockCurrentSliceIndex < (int) slices.size())
                {
                    const auto& slice = slices[(size_t) clockCurrentSliceIndex];
                    currentPosition = (double) slice.startSample;
                    currentEndSample = slice.endSample;
                    hasCurrentPick = true;
                    currentlyPlayingSliceIndexForUI.store (clockCurrentSliceIndex);

                    samplesSincePickStart = 0.0;
                    const double naturalLengthHostSamples =
                        (playbackRate > 0.0)
                            ? ((double) (slice.endSample - slice.startSample) / playbackRate)
                            : 0.0;

                    // The fade-out needs to anticipate whichever comes
                    // first — the slice's own natural end, or the forced
                    // retrigger at the next tick — otherwise a slice that
                    // gets cut short by the clock never gets a fade-out at
                    // all, and every retrigger clicks.
                    const double tickBeats = getNoteValueBeats (clockCurrentSubdivisionIndex);
                    const double tickLengthHostSamples = tickBeats * (60.0 / hostBpm) * hostSampleRate;
                    currentPickLengthInHostSamples = juce::jmin (naturalLengthHostSamples, tickLengthHostSamples);
                }
                else
                {
                    hasCurrentPick = false;
                }

                const double subdivisionBeats = getNoteValueBeats (clockCurrentSubdivisionIndex);
                nextTickPpq += juce::jmax (subdivisionBeats, 1.0e-6); // guard against a zero-length tick
                nextTickPpq = juce::jmin (nextTickPpq, windowEndPpq);
            }
        }
        else
        {
            // Slice Length mode (unchanged): pick a fresh slice whenever
            // nothing's playing or the current one has run its course.
            int pickAttempts = 0;

            while (! hasCurrentPick
                   || currentPosition >= (double) (juce::jmin (currentEndSample, sourceLength) - 1))
            {
                currentSliceIndex = pickWeightedRandomSlice();

                if (currentSliceIndex < 0 || currentSliceIndex >= (int) slices.size())
                {
                    hasCurrentPick = false;
                    break;
                }

                const auto& slice = slices[(size_t) currentSliceIndex];
                currentPosition = (double) slice.startSample;
                currentEndSample = slice.endSample;
                hasCurrentPick = true;
                currentlyPlayingSliceIndexForUI.store (currentSliceIndex);

                samplesSincePickStart = 0.0;
                const double sourceLengthOfPick = (double) (slice.endSample - slice.startSample);
                currentPickLengthInHostSamples = (playbackRate > 0.0) ? (sourceLengthOfPick / playbackRate) : 0.0;

                if (++pickAttempts > 1000)
                    return; // safety bail — render the rest of this block as silence
            }
        }

        // Shared render step for both modes: only output a sample while
        // we're within the current pick's bounds. In Clock mode, once a
        // short slice naturally finishes before the next tick, this
        // condition goes false and we correctly render silence until the
        // next forced retrigger resets currentPosition.
        if (hasCurrentPick && currentPosition < (double) (juce::jmin (currentEndSample, sourceLength) - 1))
        {
            const int idx0 = (int) currentPosition;
            const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
            const float frac = (float) (currentPosition - (double) idx0);

            // Fade gain: clamp each fade to at most half this pick's own
            // (effective) length, so a very short slice/tick can't have
            // overlapping/inverted fades that would silence it entirely.
            const double halfPickLength = currentPickLengthInHostSamples * 0.5;
            const double fadeInSamples = juce::jmin (fadeInSamplesRequested, halfPickLength);
            const double fadeOutSamples = juce::jmin (fadeOutSamplesRequested, halfPickLength);
            const double samplesRemaining = currentPickLengthInHostSamples - samplesSincePickStart;

            double gain = 1.0;

            if (fadeInSamples > 0.0 && samplesSincePickStart < fadeInSamples)
                gain = samplesSincePickStart / fadeInSamples;

            if (fadeOutSamples > 0.0 && samplesRemaining < fadeOutSamples)
                gain = juce::jmin (gain, samplesRemaining / fadeOutSamples);

            gain = juce::jlimit (0.0, 1.0, gain);

            for (int outCh = 0; outCh < outChannels; ++outCh)
            {
                const int srcCh = juce::jmin (outCh, sourceChannels - 1);
                const float s0 = sampleBuffer.getSample (srcCh, idx0);
                const float s1 = sampleBuffer.getSample (srcCh, idx1);
                const float sample = (s0 + frac * (s1 - s0)) * (float) gain;

                buffer.addSample (outCh, i, sample);
            }

            currentPosition += playbackRate;
            samplesSincePickStart += 1.0;
        }
    }
}

juce::AudioProcessorEditor* SlicerAudioProcessor::createEditor()
{
    return new SlicerAudioProcessorEditor (*this);
}

void SlicerAudioProcessor::getStateInformation (juce::MemoryBlock& /*destData*/)
{
    // TODO once there are parameters worth persisting (loop length, slice
    // probabilities) — not wired up yet in this step.
}

void SlicerAudioProcessor::setStateInformation (const void* /*data*/, int /*sizeInBytes*/)
{
}

void SlicerAudioProcessor::loadSample (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
        return;

    juce::AudioBuffer<float> newBuffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&newBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

    {
        const juce::ScopedLock sl (sampleLock);
        sampleBuffer = std::move (newBuffer);
        sampleSampleRate = reader->sampleRate;
        sampleLoaded = true;
        loadedFileName = file.getFileName();

        transientDetector.analyze (sampleBuffer, sampleSampleRate);

        manualPoints.clear(); // positions from the old sample don't mean anything here
        excludedPoints.clear();
        hasCurrentPick = false; // don't let a stale pick read past the new buffer's end
        clockModeInitialized = false;
        clockCurrentPickValid = false;
    }

    undoManager.clearUndoHistory(); // old undo steps reference positions in a different file now

    redetectSlices (defaultSensitivity, defaultHoldoffMs);
}

void SlicerAudioProcessor::redetectSlices (float sensitivity, float holdoffMs)
{
    rebuildSlicesFromDetectionAndManualPoints (sensitivity, holdoffMs);
}

void SlicerAudioProcessor::rebuildSlicesFromDetectionAndManualPoints (float sensitivity, float holdoffMs)
{
    auto autoSlices = transientDetector.detectSlices (sensitivity, holdoffMs);

    const juce::ScopedLock sl (sampleLock);

    slices = mergeOnsetsIntoSlices (autoSlices);
    sliceProbabilities.assign (slices.size(), 1.0f); // default: even odds across all slices
    hasCurrentPick = false; // boundaries changed — force a fresh pick
    clockModeInitialized = false;
    clockCurrentPickValid = false;
}

std::vector<Slice> SlicerAudioProcessor::previewSlicesAtSensitivity (float sensitivity) const
{
    auto autoSlices = transientDetector.detectSlices (sensitivity, defaultHoldoffMs);

    const juce::ScopedLock sl (sampleLock);
    return mergeOnsetsIntoSlices (autoSlices);
}

std::vector<Slice> SlicerAudioProcessor::mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices) const
{
    const int totalSamples = sampleBuffer.getNumSamples();
    const int matchToleranceSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);

    std::vector<int> onsets;
    onsets.reserve (autoSlices.size() + manualPoints.size());

    for (const auto& s : autoSlices)
    {
        if (s.startSample == 0)
        {
            onsets.push_back (0); // the very start is never excludable
            continue;
        }

        bool excluded = false;

        for (const auto& ep : excludedPoints)
        {
            if (std::abs (s.startSample - ep.samplePosition) <= matchToleranceSamples)
            {
                excluded = true;
                break;
            }
        }

        if (! excluded)
            onsets.push_back (s.startSample);
    }

    for (const auto& mp : manualPoints)
        if (mp.samplePosition > 0 && mp.samplePosition < totalSamples)
            onsets.push_back (mp.samplePosition);

    if (onsets.empty() || onsets.front() != 0)
        onsets.insert (onsets.begin(), 0);

    std::sort (onsets.begin(), onsets.end());
    onsets.erase (std::unique (onsets.begin(), onsets.end()), onsets.end());

    std::vector<Slice> result;

    for (size_t i = 0; i < onsets.size(); ++i)
    {
        Slice slice;
        slice.startSample = onsets[i];
        slice.endSample = (i + 1 < onsets.size()) ? onsets[i + 1] : totalSamples;

        if (slice.lengthInSamples() > 0)
            result.push_back (slice);
    }

    return result;
}

int SlicerAudioProcessor::addManualSlicePoint (int targetSample, bool snapToTransient)
{
    int snapped = targetSample;

    if (snapToTransient)
    {
        const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
        snapped = transientDetector.findNearestPeak (targetSample, radiusSamples);
    }

    const int id = nextManualPointId++;

    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterManual = beforeManual;
    afterManual.push_back ({ id, snapped });

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        afterManual, beforeExcluded));
    return id;
}

void SlicerAudioProcessor::moveManualSlicePoint (int id, int targetSample, bool snapToTransient)
{
    int snapped = targetSample;

    if (snapToTransient)
    {
        const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
        snapped = transientDetector.findNearestPeak (targetSample, radiusSamples);
    }

    {
        const juce::ScopedLock sl (sampleLock);

        for (auto& mp : manualPoints)
        {
            if (mp.id == id)
            {
                mp.samplePosition = snapped;
                break;
            }
        }
    }

    rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), defaultHoldoffMs);
}

void SlicerAudioProcessor::commitManualPointMove (int id, int originalSamplePosition)
{
    // The live position is already applied (moveManualSlicePoint was
    // called throughout the drag) — "after" is just the current state.
    // "before" is that same state with only this one point's position
    // put back to where the drag started.
    auto afterManual = getManualSlicePoints();
    auto beforeManual = afterManual;

    for (auto& mp : beforeManual)
        if (mp.id == id)
            mp.samplePosition = originalSamplePosition;

    auto excluded = getExcludedPoints(); // unaffected by a move

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, excluded, afterManual, excluded));
}

void SlicerAudioProcessor::removeManualSlicePoint (int id)
{
    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterManual = beforeManual;
    afterManual.erase (std::remove_if (afterManual.begin(), afterManual.end(),
                                        [id] (const ManualPointInfo& mp) { return mp.id == id; }),
                        afterManual.end());

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        afterManual, beforeExcluded));
}

int SlicerAudioProcessor::excludeNearestAutoPoint (int targetSample)
{
    // Search the raw current auto-detection result (not the merged
    // `slices`) for the nearest boundary to targetSample — position 0 is
    // never a candidate, it can't be excluded.
    auto autoSlices = transientDetector.detectSlices (currentSensitivity.load(), defaultHoldoffMs);

    int nearest = -1;
    int bestDistance = std::numeric_limits<int>::max();

    for (const auto& s : autoSlices)
    {
        if (s.startSample == 0)
            continue;

        const int distance = std::abs (s.startSample - targetSample);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            nearest = s.startSample;
        }
    }

    if (nearest < 0)
        return -1;

    const int id = nextExcludedPointId++;

    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterExcluded = beforeExcluded;
    afterExcluded.push_back ({ id, nearest });

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        beforeManual, afterExcluded));
    return id;
}

void SlicerAudioProcessor::restoreExcludedPoint (int id)
{
    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterExcluded = beforeExcluded;
    afterExcluded.erase (std::remove_if (afterExcluded.begin(), afterExcluded.end(),
                                          [id] (const ManualPointInfo& ep) { return ep.id == id; }),
                          afterExcluded.end());

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        beforeManual, afterExcluded));
}

void SlicerAudioProcessor::resetAllManualEdits()
{
    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    if (beforeManual.empty() && beforeExcluded.empty())
        return; // nothing to reset — don't pollute undo history with a no-op

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded, {}, {}));
}

void SlicerAudioProcessor::applyManualState (const std::vector<ManualPointInfo>& manual,
                                              const std::vector<ManualPointInfo>& excluded)
{
    {
        const juce::ScopedLock sl (sampleLock);

        manualPoints.clear();
        for (const auto& m : manual)
            manualPoints.push_back ({ m.id, m.samplePosition });

        excludedPoints.clear();
        for (const auto& e : excluded)
            excludedPoints.push_back ({ e.id, e.samplePosition });
    }

    rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), defaultHoldoffMs);
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SlicerAudioProcessor();
}
