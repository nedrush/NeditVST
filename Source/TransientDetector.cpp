#include "TransientDetector.h"
#include <algorithm>
#include <complex>
#include <juce_dsp/juce_dsp.h>

namespace
{
    // Envelope follower time constants — fast attack so the envelope tracks
    // transient onsets tightly, slower release so it doesn't chase every
    // sample of noise on the way down. Same shape as a standard "punch"
    // envelope follower; not exposed as a parameter yet, but easy to expose
    // later if the detection needs tuning per-source.
    constexpr float attackTimeMs = 1.0f;
    constexpr float releaseTimeMs = 50.0f;

    // TEMPORARY (Onset vs. Peak comparison tool — see DetectionMethod in
    // TransientDetector.h): much faster time constants than the pair above,
    // so the onset envelope tracks a rise almost instantly instead of
    // smoothing across ~1ms. That smoothing is exactly what the informal
    // listening comparison this tool exists for was complaining about —
    // Peak-detected slices can miss the initial attack "click" because the
    // slow envelope (and therefore its derivative crossing the threshold)
    // lags the true start of the transient by a millisecond or more.
    // Delete alongside the rest of the onset pipeline once the decision
    // is made.
    constexpr float onsetAttackTimeMs = 0.2f;
    constexpr float onsetReleaseTimeMs = 2.0f;

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

    // TEMPORARY (onset pipeline) resets — see DetectionMethod in the header.
    signedMonoSignal.clear();
    onsetEnvelope.clear();
    spectralFlux.clear();
    spectralCentroid.clear();
    spectralBinCount = fftSize / 2 + 1;
    fftDataRead.assign ((size_t) fftSize, {});
    fftDataWrite.assign ((size_t) fftSize, {});

    numSamples = buffer.getNumSamples();
    analyzedSampleRate = sampleRate;

    if (numSamples == 0)
        return;

    const int numChannels = buffer.getNumChannels();

    // Mono-sum across channels for detection purposes. Playback still uses
    // the full multichannel buffer — this is only for finding onsets.
    //
    // signedMonoSignal (TEMPORARY — onset pipeline) is the same mono-sum
    // WITHOUT rectification, computed alongside monoSum in the same pass —
    // needed later for zero-crossing snapping, which has to run against the
    // actual waveform, not a rectified/enveloped derivative of it.
    std::vector<float> monoSum (static_cast<size_t> (numSamples), 0.0f);
    signedMonoSignal.assign (static_cast<size_t> (numSamples), 0.0f);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* channelData = buffer.getReadPointer (ch);

        for (int i = 0; i < numSamples; ++i)
        {
            monoSum[(size_t) i] += std::abs (channelData[i]);
            signedMonoSignal[(size_t) i] += channelData[i];
        }
    }

    if (numChannels > 1)
    {
        const float scale = 1.0f / (float) numChannels;

        for (int i = 0; i < numSamples; ++i)
        {
            monoSum[(size_t) i] *= scale;
            signedMonoSignal[(size_t) i] *= scale;
        }
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

    // --- TEMPORARY (onset pipeline — see DetectionMethod in the header):
    // same envelope-follower + derivative shape as steps 1/2 above, run a
    // second time against the SAME rectified signal but with much shorter
    // time constants, so it isn't blurred by ~1ms of attack smoothing the
    // way the peak-picking envelope above deliberately is. ---
    onsetEnvelope.resize ((size_t) numSamples);

    const float onsetAttackCoeff = oneSampleCoeff (onsetAttackTimeMs, sampleRate);
    const float onsetReleaseCoeff = oneSampleCoeff (onsetReleaseTimeMs, sampleRate);

    float onsetEnv = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float rectified = monoSum[(size_t) i];
        const float coeff = (rectified > onsetEnv) ? onsetAttackCoeff : onsetReleaseCoeff;
        onsetEnv += coeff * (rectified - onsetEnv);
        onsetEnvelope[(size_t) i] = onsetEnv;
    }

    // --- TEMPORARY (onset pipeline): spectral layer per FAST analysis window.
    // Spectral flux (L2 norm of the positive magnitude difference between
    // adjacent frames) is the classic broadband onset detector — a transient
    // adds energy across many bins at once, so the flux spikes; the sustained
    // low-fundamental ripple that fooled the fast-envelope derivative adds
    // almost nothing frame-to-frame and stays flat. Spectral centroid (the
    // magnitude-weighted mean bin) is a coarse spectral-position cue: attacks
    // tend to be broadband (high centroid) while pitches / tails are narrow.
    // Both arrays are one-per-FRAME (hop-size resolution) — coarse vs. the
    // per-sample envelopes; callers index by sample / hop. Delete with the
    // onset pipeline once the right feature is validated and wired in.
    const int hopSize = fftHopSamples;
    const int numFrames = juce::jmax (0, (numSamples - fftSize + hopSize) / hopSize);

    spectralFlux.resize ((size_t) numFrames, 0.0f);
    spectralCentroid.resize ((size_t) numFrames, 0.0f);

    std::vector<float> prevMag (spectralBinCount, 0.0f);
    std::vector<float> window (static_cast<size_t> (fftSize));

    for (int w = 0; w < fftSize; ++w)
        window[(size_t) w] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * (float) w / (float) (fftSize - 1)));

    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int center = frame * hopSize;

        std::copy_n (signedMonoSignal.data() + center, fftSize, fftDataRead.data());

        for (int w = 0; w < fftSize; ++w)
            fftDataRead[(size_t) w] *= window[(size_t) w];

        fft.perform (fftDataRead.data(), fftDataWrite.data(), false);

        double curFlux = 0.0;
        double curCentroid = 0.0;
        double totalMag = 0.0;

        for (int b = 0; b < spectralBinCount; ++b)
        {
            const float re = fftDataWrite[(size_t) b].real();
            const float im = fftDataWrite[(size_t) b].imag();
            const float mag = std::sqrt (re * re + im * im);
            const float diff = mag - prevMag[(size_t) b];

            if (diff > 0.0f)
                curFlux += diff * diff;

            curCentroid += (float) b * mag;
            totalMag += mag;
            prevMag[(size_t) b] = mag;
        }

        spectralFlux[(size_t) frame] = (float) std::sqrt (curFlux);
        spectralCentroid[(size_t) frame] = totalMag > 0.0 ? (float) (curCentroid / totalMag) : 0.0f;
    }
}

std::vector<int> TransientDetector::pickPeakOnsets (float sensitivity, float holdoffMs,
                                                      int rangeStartSample, int rangeEndSample) const
{
    std::vector<int> onsets;

    // sensitivity == 0 is guaranteed zero transients, same contract as the
    // JS version — skip straight to "whole range is one slice" below.
    if (sensitivity <= 0.0f)
        return onsets;

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

    return onsets;
}

// TEMPORARY (Onset vs. Peak comparison tool — see DetectionMethod in the
// header). HYBRID detector: the slow peak-pipeline derivative decides THAT
// a hit happened, the fast onset envelope decides WHERE to cut.
//
// Detection previously thresholded the FAST envelope's derivative, but that
// envelope's 2ms release ripples with every cycle of low-frequency material
// (a 60Hz fundamental has a 16ms period), so every cycle's rise was a false-
// positive candidate. The slow envelope (1ms/50ms) is structurally immune to
// per-cycle ripple, so its derivative crossings — identical threshold and
// holdoff logic to pickPeakOnsets, so detection COUNT matches Peak exactly —
// pick the hits. Placement then uses the fast envelope, which the slow one
// is too blurred for: find the fast attack peak near the slow rise, take the
// deepest fast valley before that peak (the true start of the attack), and
// snap to the nearest zero-crossing for a click-free cut.
//
// The spectral features computed in analyze() (flux / centroid) are exposed
// for the debug overlay; once the right feature is validated against real
// material it becomes the confirmation gate for the crossings here.
std::vector<int> TransientDetector::pickOnsetOnsets (float sensitivity, float holdoffMs,
                                                       int rangeStartSample, int rangeEndSample) const
{
    std::vector<int> onsets;

    if (sensitivity <= 0.0f)
        return onsets;

    // Same threshold as pickPeakOnsets — the slow pipeline's statistics.
    const float threshold = globalMaxDerivative
                             - sensitivity * (globalMaxDerivative - noiseFloor);

    const int holdoffSamples = (int) ((holdoffMs / 1000.0f) * (float) analyzedSampleRate);
    const int zeroCrossingSearchRadius = (int) (0.002 * analyzedSampleRate); // ~2ms
    const int placementPadSamples = (int) (0.001 * analyzedSampleRate); // ~1ms of slack around the slow rise's base

    int lastOnset = rangeStartSample - holdoffSamples;
    int lastFinalOnset = rangeStartSample - 1; // guards monotonic ordering after the placement/snap below can shift a crossing earlier than the previous one's

    for (int i = juce::jmax (1, rangeStartSample); i < rangeEndSample; ++i)
    {
        if (derivative[(size_t) i] > threshold
            && derivative[(size_t) (i - 1)] <= threshold
            && (i - lastOnset) >= holdoffSamples)
        {
            const int walkBackLimit = juce::jmax (rangeStartSample, lastFinalOnset + 1);

            // 1) Walk the SLOW envelope's mirrored rise back to its base —
            // a stable anchor near the attack (the slow envelope barely
            // ripples, so this can't get lost the way the fast one could).
            int slowBase = i;

            while (slowBase > walkBackLimit
                   && envelope[(size_t) (slowBase - 1)] < envelope[(size_t) slowBase])
                --slowBase;

            // Rise began at/before the previous cut: this crossing is the
            // tail of the previous onset's own rise, not an independent
            // transient — skip rather than pin it into a 1-sample slice.
            if (slowBase == walkBackLimit)
                continue;

            // 2) Fast attack peak near the slow rise. The slow crossing lags
            // the true attack (1ms attack smoothing plus however far up the
            // rise the threshold sits), so the fast envelope may already be
            // past its peak — search the window rather than walking blindly.
            const int windowStart = juce::jmax (walkBackLimit, slowBase - placementPadSamples);
            int fastPeak = windowStart;

            for (int s = windowStart + 1; s <= i; ++s)
                if (onsetEnvelope[(size_t) s] > onsetEnvelope[(size_t) fastPeak])
                    fastPeak = s;

            // 3) Deepest fast valley before the attack peak = the true start
            // of the attack. Last occurrence of the minimum, so the cut hugs
            // the attack rather than sitting at the far edge of a flat trough.
            int riseStart = windowStart;

            for (int s = windowStart + 1; s <= fastPeak; ++s)
                if (onsetEnvelope[(size_t) s] <= onsetEnvelope[(size_t) riseStart])
                    riseStart = s;

            const int snapped = snapToNearestZeroCrossing (riseStart, zeroCrossingSearchRadius);
            const int lowBound = juce::jmax (rangeStartSample, lastFinalOnset + 1);

            // The snap can only move ~2ms, but if it (or the placement)
            // landed at/before the previous cut, clamping would recreate the
            // 1-sample slices — skip the degenerate case instead. (Clamping
            // TO the range start is fine: that boundary always exists.)
            if (snapped < lowBound && lowBound > rangeStartSample)
                continue;

            const int finalOnset = juce::jlimit (lowBound, rangeEndSample - 1, snapped);

            onsets.push_back (finalOnset);
            lastFinalOnset = finalOnset;
            lastOnset = i; // holdoff is measured from the raw crossing, not the (earlier) placed/snapped position
        }
    }

    return onsets;
}

int TransientDetector::snapToNearestZeroCrossing (int index, int searchRadiusSamples) const
{
    if (signedMonoSignal.empty())
        return index;

    const int lo = juce::jmax (1, index - searchRadiusSamples);
    const int hi = juce::jmin (numSamples - 1, index + searchRadiusSamples);

    int bestIndex = index;
    int bestDistance = -1;

    for (int i = lo; i <= hi; ++i)
    {
        const float prev = signedMonoSignal[(size_t) (i - 1)];
        const float curr = signedMonoSignal[(size_t) i];

        if ((prev <= 0.0f && curr >= 0.0f) || (prev >= 0.0f && curr <= 0.0f))
        {
            const int distance = (i > index) ? (i - index) : (index - i);

            if (bestDistance < 0 || distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = i;
            }
        }
    }

    return bestIndex;
}

std::vector<Slice> TransientDetector::detectSlices (float sensitivity, float holdoffMs,
                                                      int rangeStartSample, int rangeEndSample,
                                                      DetectionMethod method) const
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

    std::vector<int> onsets = (method == DetectionMethod::onset)
                                   ? pickOnsetOnsets (sensitivity, holdoffMs, rangeStartSample, rangeEndSample)
                                   : pickPeakOnsets (sensitivity, holdoffMs, rangeStartSample, rangeEndSample);

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
