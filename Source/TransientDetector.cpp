#include "TransientDetector.h"

namespace
{
    // Envelope follower time constants — fast attack so the envelope tracks
    // transient onsets tightly, slower release so it doesn't chase every
    // sample of noise on the way down. Same shape as a standard "punch"
    // envelope follower; not exposed as a parameter yet, but easy to expose
    // later if the detection needs tuning per-source.
    constexpr float attackTimeMs = 1.0f;
    constexpr float releaseTimeMs = 50.0f;

    float oneSampleCoeff (float timeMs, double sampleRate)
    {
        if (timeMs <= 0.0f)
            return 1.0f;

        const double timeSeconds = (double) timeMs / 1000.0;
        return (float) (1.0 - std::exp (-1.0 / (timeSeconds * sampleRate)));
    }
}

void TransientDetector::analyze (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    envelope.clear();
    derivative.clear();
    globalMaxDerivative = 0.0f;
    noiseFloor = 0.0f;

    numSamples = buffer.getNumSamples();
    analyzedSampleRate = sampleRate;

    if (numSamples == 0)
        return;

    const int numChannels = buffer.getNumChannels();

    // Mono-sum across channels for detection purposes. Playback still uses
    // the full multichannel buffer — this is only for finding onsets.
    std::vector<float> monoSum (static_cast<size_t> (numSamples), 0.0f);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* channelData = buffer.getReadPointer (ch);

        for (int i = 0; i < numSamples; ++i)
            monoSum[(size_t) i] += std::abs (channelData[i]);
    }

    if (numChannels > 1)
    {
        const float scale = 1.0f / (float) numChannels;
        for (auto& s : monoSum)
            s *= scale;
    }

    // --- 1) Rectified envelope follower (asymmetric attack/release) ---
    envelope.resize ((size_t) numSamples);

    const float attackCoeff = oneSampleCoeff (attackTimeMs, sampleRate);
    const float releaseCoeff = oneSampleCoeff (releaseTimeMs, sampleRate);

    float env = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float rectified = monoSum[(size_t) i];
        const float coeff = (rectified > env) ? attackCoeff : releaseCoeff;
        env += coeff * (rectified - env);
        envelope[(size_t) i] = env;
    }

    // --- 2) Derivative of the envelope (only rising edges matter) ---
    derivative.resize ((size_t) numSamples, 0.0f);

    double sumPositiveDerivative = 0.0;
    int numPositiveDerivative = 0;

    for (int i = 1; i < numSamples; ++i)
    {
        const float d = envelope[(size_t) i] - envelope[(size_t) (i - 1)];
        const float positiveD = juce::jmax (0.0f, d);
        derivative[(size_t) i] = positiveD;

        if (positiveD > globalMaxDerivative)
            globalMaxDerivative = positiveD;

        if (positiveD > 0.0f)
        {
            sumPositiveDerivative += positiveD;
            ++numPositiveDerivative;
        }
    }

    // --- Noise floor estimate: mean of the positive derivative values.
    // A simple global estimate for now — good enough to make sensitivity=1
    // "maximally permissive" without drowning in noise-level triggers.
    // If quiet-passage-vs-loud-passage material turns out to need a
    // windowed/local floor instead of one global figure, this is the spot
    // to revisit.
    noiseFloor = (numPositiveDerivative > 0)
                     ? (float) (sumPositiveDerivative / (double) numPositiveDerivative)
                     : 0.0f;
}

std::vector<Slice> TransientDetector::detectSlices (float sensitivity, float holdoffMs,
                                                      int rangeStartSample, int rangeEndSample) const
{
    std::vector<Slice> slices;

    if (! hasAnalysis() || numSamples == 0)
        return slices;

    // -1 sentinel (the default) means "the whole analysed buffer" — matches
    // every pre-trim caller's behaviour exactly.
    if (rangeStartSample < 0) rangeStartSample = 0;
    if (rangeEndSample < 0) rangeEndSample = numSamples;

    rangeStartSample = juce::jlimit (0, numSamples, rangeStartSample);
    rangeEndSample = juce::jlimit (rangeStartSample, numSamples, rangeEndSample);

    sensitivity = juce::jlimit (0.0f, 1.0f, sensitivity);

    std::vector<int> onsets;

    // sensitivity == 0 is guaranteed zero transients, same contract as the
    // JS version — skip straight to "whole range is one slice" below.
    if (sensitivity > 0.0f)
    {
        const float threshold = globalMaxDerivative
                                 - sensitivity * (globalMaxDerivative - noiseFloor);

        const int holdoffSamples = (int) ((holdoffMs / 1000.0f) * (float) analyzedSampleRate);
        int lastOnset = rangeStartSample - holdoffSamples; // allow an onset right at the range start

        // Starts at max(1, rangeStartSample) since the derivative comparison
        // below needs a valid i-1; matches the pre-trim loop exactly when
        // rangeStartSample is 0.
        for (int i = juce::jmax (1, rangeStartSample); i < rangeEndSample; ++i)
        {
            if (derivative[(size_t) i] > threshold
                && derivative[(size_t) i] >= derivative[(size_t) (i - 1)]
                && (i - lastOnset) >= holdoffSamples)
            {
                onsets.push_back (i);
                lastOnset = i;
            }
        }
    }

    // Make sure nothing before the first detected onset gets orphaned —
    // the range start plays the role position 0 used to play pre-trim.
    if (onsets.empty() || onsets.front() > rangeStartSample)
        onsets.insert (onsets.begin(), rangeStartSample);

    for (size_t i = 0; i < onsets.size(); ++i)
    {
        Slice slice;
        slice.startSample = onsets[i];
        slice.endSample = (i + 1 < onsets.size()) ? onsets[i + 1] : rangeEndSample;

        if (slice.lengthInSamples() > 0)
            slices.push_back (slice);
    }

    return slices;
}

int TransientDetector::findNearestPeak (int targetSample, int searchRadiusSamples,
                                         int rangeStartSample, int rangeEndSample) const
{
    if (! hasAnalysis() || numSamples == 0)
        return targetSample;

    if (rangeStartSample < 0) rangeStartSample = 0;
    if (rangeEndSample < 0) rangeEndSample = numSamples;

    rangeStartSample = juce::jlimit (0, numSamples, rangeStartSample);
    rangeEndSample = juce::jlimit (rangeStartSample, numSamples, rangeEndSample);

    if (rangeEndSample <= rangeStartSample)
        return juce::jlimit (0, numSamples - 1, targetSample); // degenerate range — nothing to search

    const int rangeLastIndex = rangeEndSample - 1;
    const int lo = juce::jlimit (rangeStartSample, rangeLastIndex, targetSample - searchRadiusSamples);
    const int hi = juce::jlimit (rangeStartSample, rangeLastIndex, targetSample + searchRadiusSamples);

    int bestIndex = juce::jlimit (rangeStartSample, rangeLastIndex, targetSample);
    float bestValue = -1.0f;

    for (int i = lo; i <= hi; ++i)
    {
        if (derivative[(size_t) i] > bestValue)
        {
            bestValue = derivative[(size_t) i];
            bestIndex = i;
        }
    }

    return bestIndex;
}
