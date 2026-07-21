#pragma once

#include <JuceHeader.h>
#include <vector>

//==============================================================================
// STEP 2 of the build: the transient detector, ported from the JS prototype
// we built and validated in the Max/MSP chat.
//
// Same pipeline as the Max version:
//   1) rectified envelope follower (fast attack, slower release)
//   2) derivative of the envelope
//   3) adaptive threshold, scaled by a 0-1 sensitivity control
//        sensitivity 0 -> threshold sits just above the global max
//                          derivative spike, so nothing crosses it
//                          (zero transients — matches the JS behaviour)
//        sensitivity 1 -> threshold drops to the local noise floor
//                          (maximally permissive)
//   4) peak-picking with a holdoff so one transient doesn't fire twice
//
// Two-stage split (also carried over from the Max version): `analyze()` does
// the expensive one-off work (envelope + derivative across the whole file)
// and caches it; `detectSlices()` cheaply re-runs just the thresholding and
// peak-picking against the cached derivative, so re-running detection with a
// different sensitivity/holdoff doesn't require re-analysing the audio.
//==============================================================================

struct Slice
{
    int startSample = 0;
    int endSample = 0; // exclusive

    int lengthInSamples() const { return endSample - startSample; }
};

class TransientDetector
{
public:
    /** Runs the envelope + derivative pass once and caches the results.
        Call this whenever a new sample is loaded. Mono-sums multichannel
        buffers for detection purposes (playback still uses all channels). */
    void analyze (const juce::AudioBuffer<float>& buffer, double sampleRate);

    /** Cheap re-run of thresholding + peak-picking against the cached
        derivative. Safe to call repeatedly (e.g. while dragging a
        sensitivity slider in the UI) without re-analysing the audio.

        sensitivity: 0.0 (nothing detected) .. 1.0 (maximally permissive)
        holdoffMs:   minimum gap between consecutive onsets
        rangeStartSample/rangeEndSample (Step 23 — trim markers): confines
        both the onset search AND the returned slices to
        [rangeStartSample, rangeEndSample) — nothing outside a trimmed
        range is ever detected or becomes a slice. rangeStartSample takes
        the role position 0 used to play (the one always-present, never-
        excludable boundary); the last slice's endSample is rangeEndSample
        rather than the buffer's true length. Defaults (-1, -1) mean "the
        whole analysed buffer," matching pre-trim behaviour exactly. */
    std::vector<Slice> detectSlices (float sensitivity, float holdoffMs,
                                      int rangeStartSample = -1, int rangeEndSample = -1) const;

    bool hasAnalysis() const { return ! derivative.empty(); }
    int getAnalyzedLengthInSamples() const { return numSamples; }

    /** Manual slice points (Step 10) snap to this — searches the cached
        derivative curve within +/- searchRadiusSamples of targetSample and
        returns the index of the strongest nearby transient-like peak, even
        if it's below the current sensitivity threshold. This is exactly
        what "the transient that would have been detected at higher
        sensitivity" means: the data was there in the derivative all along,
        sensitivity just decides where the cutoff line sits. Returns
        targetSample unchanged if there's no analysis to search.

        rangeStartSample/rangeEndSample (Step 23 — trim markers): the
        search (and its result) is additionally clamped to
        [rangeStartSample, rangeEndSample) — a manual point can never snap
        to a peak outside the trimmed range. Defaults (-1, -1) mean "the
        whole analysed buffer." */
    int findNearestPeak (int targetSample, int searchRadiusSamples,
                          int rangeStartSample = -1, int rangeEndSample = -1) const;

private:
    std::vector<float> envelope;
    std::vector<float> derivative;
    double analyzedSampleRate = 44100.0;
    int numSamples = 0;
    float globalMaxDerivative = 0.0f;
    float noiseFloor = 0.0f;
};
