#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cstdlib>
#include <limits>

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
}

//==============================================================================
SlicerAudioProcessor::SlicerAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();
}

SlicerAudioProcessor::~SlicerAudioProcessor() = default;

void SlicerAudioProcessor::prepareToPlay (double /*sampleRate*/, int /*samplesPerBlock*/)
{
    hasCurrentPick = false;
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
    // faster than host (slow down, pitch drops).
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

    for (int i = 0; i < numSamples; ++i)
    {
        // Advance the chain: if nothing's currently playing, or the current
        // pick has run out, pick a new slice via weighted draw. A guard
        // against a pathological all-zero-length-slice scenario stalling
        // forever — extremely unlikely in practice (the detector never
        // produces zero-length slices) but cheap insurance.
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

            // Fade tracking is in host-output-sample units so a fade stays
            // a constant number of milliseconds regardless of repitching.
            samplesSincePickStart = 0.0;
            const double sourceLengthOfPick = (double) (slice.endSample - slice.startSample);
            currentPickLengthInHostSamples = (playbackRate > 0.0) ? (sourceLengthOfPick / playbackRate) : 0.0;

            if (++pickAttempts > 1000)
                return; // safety bail — render the rest of this block as silence
        }

        if (! hasCurrentPick)
            return;

        const int idx0 = (int) currentPosition;
        const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
        const float frac = (float) (currentPosition - (double) idx0);

        // Fade gain: clamp each fade to at most half this pick's own
        // length, so a very short slice can't get overlapping/inverted
        // fades that would silence it entirely.
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
