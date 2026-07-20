#pragma once

#include <JuceHeader.h>
#include "TransientDetector.h"

//==============================================================================
// STEP 6/8: transport-synced generative playback — no MIDI, no keyboard.
//
// How it works (Step 8 revision — self-chaining weighted playback):
//   - The loaded sample is treated as `loopLengthBars` bars long (set by the
//     user). That, plus the sample's actual length, gives us its original
//     tempo, which is repitched (varispeed — pitch follows speed) to match
//     whatever tempo the host is running at.
//   - There is no fixed clock grid. Instead: pick one slice via weighted
//     random draw (weights = the per-slice probability sliders), play it in
//     full at its own (repitched) length, and the INSTANT it finishes, pick
//     again. A slice's own duration is what paces the next decision — a
//     slice with the only nonzero weight simply repeats back-to-back.
//   - This runs continuously the whole time the host transport is playing;
//     there's no bar-boundary resync. Weight 0 = that slice is excluded
//     from the draw entirely (never picked, though the math still tolerates
//     it fine even without exclusion).
//==============================================================================

//==============================================================================
class SlicerAudioProcessor : public juce::AudioProcessor
{
public:
    SlicerAudioProcessor();
    ~SlicerAudioProcessor() override;

    //=== Standard AudioProcessor overrides ===
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; } // transport-driven now, no MIDI needed
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //=== Sample loading (called from the editor) ===
    void loadSample (const juce::File& file);
    bool hasSample() const { return sampleLoaded; }
    juce::String getLoadedFileName() const { return loadedFileName; }
    const juce::AudioBuffer<float>& getSampleBuffer() const { return sampleBuffer; }

    //=== Slicing ===
    void redetectSlices (float sensitivity, float holdoffMs);
    int getNumSlices() const { return (int) slices.size(); }
    Slice getSlice (int index) const { return slices[(size_t) index]; }
    const std::vector<Slice>& getSlices() const { return slices; }

    // Live sensitivity control — was hardcoded until now. Re-runs detection
    // immediately (cheap, since TransientDetector caches the expensive
    // envelope/derivative pass) and resets slice probabilities to 1.0, same
    // as any other re-slice, since the slice boundaries themselves change.
    void setSensitivityAndRedetect (float sensitivity)
    {
        currentSensitivity.store (juce::jlimit (0.0f, 1.0f, sensitivity));
        redetectSlices (currentSensitivity.load(), defaultHoldoffMs);
    }

    float getSensitivity() const { return currentSensitivity.load(); }

    // Live preview (Step 12): shows what detection WOULD produce at a
    // given sensitivity — merged with the current manual/excluded points,
    // same as a real commit — but without touching playback state at all
    // (no probability reset, no interrupting the current pick, not added
    // to undo history). Safe to call repeatedly while a slider is being
    // dragged; the real commit only happens via setSensitivityAndRedetect().
    std::vector<Slice> previewSlicesAtSensitivity (float sensitivity) const;

    //=== Manual slice points (Step 10) ===
    // User-placed slice boundaries, layered on top of whatever the
    // detector finds automatically. Unlike auto-detected slices, these
    // survive a sensitivity change — redetection only regenerates the
    // auto side and re-merges it with whatever manual points already
    // exist. Each point snaps to the nearest real transient-like peak in
    // the cached derivative curve (via TransientDetector::findNearestPeak),
    // even one below the current sensitivity threshold.
    struct ManualPointInfo
    {
        int id = -1;
        int samplePosition = 0;
    };

    // Adds a new manual point near targetSample. Snaps to the nearest
    // real transient-like peak by default; pass snapToTransient = false
    // (Shift held) to place it at the exact position instead. Returns its
    // stable id, used later to move or remove it.
    int addManualSlicePoint (int targetSample, bool snapToTransient = true);

    // Moves an existing manual point (by id) to a new target. Snaps by
    // default; pass snapToTransient = false (Shift held) for free
    // placement at the exact position. Deliberately NOT undo-tracked —
    // this is called continuously while the user drags a point, and we
    // don't want one undo step per pixel. Call commitManualPointMove()
    // once, at drag-end, to record the whole drag as a single undoable
    // step.
    void moveManualSlicePoint (int id, int targetSample, bool snapToTransient = true);

    // Records a completed drag (from originalSamplePosition to wherever
    // the point currently is) as one undo step. Call this on mouse-up.
    void commitManualPointMove (int id, int originalSamplePosition);

    void removeManualSlicePoint (int id);

    std::vector<ManualPointInfo> getManualSlicePoints() const
    {
        const juce::ScopedLock sl (sampleLock);
        std::vector<ManualPointInfo> result;
        result.reserve (manualPoints.size());

        for (const auto& mp : manualPoints)
            result.push_back ({ mp.id, mp.samplePosition });

        return result;
    }

    //=== Deleting auto-detected transients (Step 11) ===
    // "Deletes" the nearest auto-detected boundary to targetSample by
    // adding it to an exclusion list — matched by proximity (same
    // tolerance as manual-point snapping) rather than exact position, so
    // a later sensitivity tweak that shifts the detected position by a
    // sample or two doesn't silently un-exclude it. Position 0 (the very
    // start of the sample) can never be excluded. Returns the new
    // exclusion's id, or -1 if there was nothing nearby to exclude.
    int excludeNearestAutoPoint (int targetSample);

    // Un-deletes a single excluded point.
    void restoreExcludedPoint (int id);

    std::vector<ManualPointInfo> getExcludedPoints() const
    {
        const juce::ScopedLock sl (sampleLock);
        std::vector<ManualPointInfo> result;
        result.reserve (excludedPoints.size());

        for (const auto& ep : excludedPoints)
            result.push_back ({ ep.id, ep.samplePosition });

        return result;
    }

    // Safety net: clears every manual addition AND every exclusion in one
    // go, back to exactly what the detector alone would produce at the
    // current sensitivity. Undo-tracked like everything else in this
    // section — one Undo click brings it all back if this was a mistake.
    void resetAllManualEdits();

    //=== Undo/redo (Step 12) ===
    // Covers manual point add/move/remove and auto-point exclude/restore
    // (including Reset) — every slice-editing action, as one coalesced
    // step each. Deliberately does NOT cover sensitivity, probability
    // sliders, loop length, or fades — those are continuous parameters,
    // not discrete "actions," and including them would flood the history
    // with noise from every pixel of a drag.
    bool undoLastEdit() { return undoManager.undo(); }
    bool redoLastEdit() { return undoManager.redo(); }
    bool canUndoEdit() const { return undoManager.canUndo(); }
    bool canRedoEdit() const { return undoManager.canRedo(); }

    // Overwrites manual + excluded point state wholesale and rebuilds —
    // the one place all undo/redo actions actually apply a snapshot.
    // Public because the undo action objects (defined in the .cpp) need
    // to call it; not intended to be called directly from the UI.
    void applyManualState (const std::vector<ManualPointInfo>& manual,
                            const std::vector<ManualPointInfo>& excluded);

    //=== Currently-playing slice (Step 11) ===
    // For UI highlighting — which slice is sounding right now, updated by
    // the audio thread every time a new pick begins. -1 when nothing's
    // playing (including whenever the transport is stopped).
    int getCurrentlyPlayingSliceIndex() const { return currentlyPlayingSliceIndexForUI.load(); }

    //=== Loop length / tempo sync ===
    // How many bars (assumed 4/4) the loaded sample represents. This is
    // what lets us calculate the sample's original tempo and therefore how
    // much to repitch it to match the host.
    void setLoopLengthBars (int bars) { loopLengthBars.store (juce::jmax (1, bars)); }
    int getLoopLengthBars() const { return loopLengthBars.load(); }

    // Calculated from loopLengthBars + the sample's actual length. Exposed
    // mainly so the editor can display it — "this loop is ~140 BPM".
    double getCalculatedOriginalBpm() const
    {
        if (! sampleLoaded || sampleBuffer.getNumSamples() == 0)
            return 0.0;

        const double lengthSeconds = (double) sampleBuffer.getNumSamples() / sampleSampleRate;
        const double beats = (double) loopLengthBars.load() * 4.0; // assumes 4/4
        return (beats * 60.0) / lengthSeconds;
    }

    //=== Per-slice weight (Step 8) ===
    // Relative weight in the weighted-random draw that picks the next
    // slice to play — NOT an independent per-hit probability anymore.
    // 0 = excluded from the draw entirely. Higher = more likely relative
    // to the other slices' weights. Defaults to 1.0 (even odds across all
    // slices) on every re-slice.
    float getSliceProbability (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) sliceProbabilities.size())
            return 1.0f;

        return sliceProbabilities[(size_t) index];
    }

    void setSliceProbability (int index, float probability)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) sliceProbabilities.size())
            sliceProbabilities[(size_t) index] = juce::jlimit (0.0f, 1.0f, probability);
    }

    //=== De-clicking (Step 9) ===
    // Global fade-in/fade-out applied at the start/end of every slice
    // pick, in milliseconds — real time, independent of repitching, so a
    // slice played twice as fast still gets the same-length fade. Clamped
    // per-pick to at most half that pick's own length so a very short
    // slice can't have overlapping/inverted fades. Feedback from the
    // original Nedit device was that vocal material especially clicked
    // without this; drum/percussion material is the main target here but
    // the fix is free either way.
    void setFadeInMs (float ms) { fadeInMs.store (juce::jmax (0.0f, ms)); }
    float getFadeInMs() const { return fadeInMs.load(); }

    void setFadeOutMs (float ms) { fadeOutMs.store (juce::jmax (0.0f, ms)); }
    float getFadeOutMs() const { return fadeOutMs.load(); }

private:
    // Weighted-random pick across all slices, using sliceProbabilities as
    // weights. Falls back to uniform-random if every weight is 0 (rather
    // than picking nothing and stalling the chain).
    int pickWeightedRandomSlice()
    {
        float totalWeight = 0.0f;

        for (auto w : sliceProbabilities)
            totalWeight += juce::jmax (0.0f, w);

        if (totalWeight <= 0.0f)
            return random.nextInt ((int) slices.size());

        const float target = random.nextFloat() * totalWeight;
        float cumulative = 0.0f;

        for (size_t i = 0; i < sliceProbabilities.size(); ++i)
        {
            cumulative += juce::jmax (0.0f, sliceProbabilities[i]);

            if (target <= cumulative)
                return (int) i;
        }

        return (int) sliceProbabilities.size() - 1; // float rounding fallback
    }

    // Shared by redetectSlices() and every manual-point mutation: re-runs
    // auto-detection at the given sensitivity, merges the result with the
    // current manual points, sorts + dedupes into one boundary list, and
    // rebuilds `slices` from it. Slice probabilities reset to 1.0 whenever
    // this runs — same known simplification as before Step 10, since
    // slice indices shift around whenever boundaries are added/removed
    // and there's no stable identity to carry a probability value across.
    void rebuildSlicesFromDetectionAndManualPoints (float sensitivity, float holdoffMs);

    // Pure merge logic (no side effects, no member writes) shared by the
    // real rebuild above and previewSlicesAtSensitivity(). Takes a raw
    // auto-detection result and folds in exclusions + manual points.
    // Must be called with sampleLock already held.
    std::vector<Slice> mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices) const;

    struct ManualSlicePoint
    {
        int id = -1;
        int samplePosition = 0;
    };

    std::vector<ManualSlicePoint> manualPoints;
    int nextManualPointId = 1;

    struct ExcludedPoint
    {
        int id = -1;
        int samplePosition = 0;
    };

    std::vector<ExcludedPoint> excludedPoints;
    int nextExcludedPointId = 1;

    juce::UndoManager undoManager;

    // Search window for snapping a manual point to the nearest real
    // transient-like peak — generous enough to catch "the hit that's
    // obviously there" without snapping across to an unrelated one.
    static constexpr float manualSnapRadiusMs = 50.0f;

    juce::AudioFormatManager formatManager;

    juce::AudioBuffer<float> sampleBuffer;
    double sampleSampleRate = 44100.0;
    bool sampleLoaded = false;
    juce::String loadedFileName;
    juce::CriticalSection sampleLock; // guards sampleBuffer/slices during loadSample()

    TransientDetector transientDetector;
    std::vector<Slice> slices;
    std::vector<float> sliceProbabilities; // parallel to slices; reset to 1.0 each on redetectSlices()
    juce::Random random;

    std::atomic<int> loopLengthBars { 1 };
    std::atomic<float> currentSensitivity { defaultSensitivity };
    std::atomic<float> fadeInMs { 5.0f };
    std::atomic<float> fadeOutMs { 15.0f };

    // Self-chaining playback state — which slice is currently sounding,
    // where we are within it (source sample units), and where it ends.
    // When position reaches the end, the very next sample immediately
    // picks a new slice and continues with zero gap.
    bool hasCurrentPick = false;
    int currentSliceIndex = -1;
    double currentPosition = 0.0;
    int currentEndSample = 0;

    // Lock-free copy of currentSliceIndex, written by the audio thread
    // whenever a new pick begins, read by the UI thread for the playhead
    // highlight. Separate from currentSliceIndex itself so the UI never
    // needs to touch sampleLock just to poll this at 30fps.
    std::atomic<int> currentlyPlayingSliceIndexForUI { -1 };

    // Fade tracking, in host-output-sample units (not source-sample units,
    // so fade length in ms stays constant regardless of repitching).
    // Reset every time a new pick starts.
    double samplesSincePickStart = 0.0;
    double currentPickLengthInHostSamples = 0.0;

    // Sensible starting defaults — moderate sensitivity, 30ms holdoff to
    // avoid double-triggering on a single drum hit's ringing tail.
    static constexpr float defaultSensitivity = 0.5f;
    static constexpr float defaultHoldoffMs = 30.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessor)
};
