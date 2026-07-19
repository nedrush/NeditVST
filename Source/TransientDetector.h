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
        holdoffMs:   minimum gap between consecutive onsets */
    std::vector<Slice> detectSlices (float sensitivity, float holdoffMs) const;

    bool hasAnalysis() const { return ! derivative.empty(); }
    int getAnalyzedLengthInSamples() const { return numSamples; }

private:
    std::vector<float> envelope;
    std::vector<float> derivative;
    double analyzedSampleRate = 44100.0;
    int numSamples = 0;
    float globalMaxDerivative = 0.0f;
    float noiseFloor = 0.0f;
};
