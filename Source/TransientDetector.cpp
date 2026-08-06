#include "TransientDetector.h"
#include <algorithm>

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
    onsetDerivative.clear();
    onsetGlobalMaxDerivative = 0.0f;
    onsetNoiseFloor = 0.0f;
    onsetGlobalMaxAmplitude = 0.0f;

    numSamples = buffer.getNumSamples();
    analyzedSampleRate = sampleRate;

    for (auto& c : onsetCache)
        c.valid = false; // a new buffer invalidates all cached onsets

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

        if (onsetEnv > onsetGlobalMaxAmplitude)
            onsetGlobalMaxAmplitude = onsetEnv;
    }

    onsetDerivative.resize ((size_t) numSamples, 0.0f);

    double sumPositiveOnsetDerivative = 0.0;
    int numPositiveOnsetDerivative = 0;

    for (int i = 1; i < numSamples; ++i)
    {
        const float d = onsetEnvelope[(size_t) i] - onsetEnvelope[(size_t) (i - 1)];
        const float positiveD = juce::jmax (0.0f, d);
        onsetDerivative[(size_t) i] = positiveD;

        if (positiveD > onsetGlobalMaxDerivative)
            onsetGlobalMaxDerivative = positiveD;

        if (positiveD > 0.0f)
        {
            sumPositiveOnsetDerivative += positiveD;
            ++numPositiveOnsetDerivative;
        }
    }

    onsetNoiseFloor = (numPositiveOnsetDerivative > 0)
                          ? (float) (sumPositiveOnsetDerivative / (double) numPositiveOnsetDerivative)
                          : 0.0f;
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
// header). Finds where the fast onset-derivative first crosses the
// sensitivity-scaled threshold (a much snappier signal than the peak
// pipeline's slow-envelope derivative), then walks that crossing BACKWARD
// to the last sample the fast envelope was still at-or-below a "near
// silence" floor — i.e. the actual start of the rise, not the point an
// arbitrary threshold happened to be crossed — and finally snaps to the
// nearest zero-crossing in the raw signal for a click-free cut. Delete
// alongside the rest of the onset pipeline once the decision is made.
std::vector<int> TransientDetector::pickOnsetOnsets (float sensitivity, float holdoffMs,
                                                       int rangeStartSample, int rangeEndSample) const
{
    std::vector<int> onsets;

    if (sensitivity <= 0.0f)
        return onsets;

    const float threshold = onsetGlobalMaxDerivative
                             - sensitivity * (onsetGlobalMaxDerivative - onsetNoiseFloor);

    // "Near silence" for the walk-back below: 5% of the loudest point in
    // the whole analysed buffer. An amplitude floor, not a derivative one
    // (onsetNoiseFloor is in derivative units) — walking back is about
    // "has the level dropped to nothing," not "has the rate of change."
    const float silenceFloor = 0.05f * onsetGlobalMaxAmplitude;

    const int holdoffSamples = (int) ((holdoffMs / 1000.0f) * (float) analyzedSampleRate);
    const int maxWalkBackSamples = (int) (0.03 * analyzedSampleRate); // ~30ms cap -- avoids a runaway backward search on pathological/DC-heavy material
    const int zeroCrossingSearchRadius = (int) (0.002 * analyzedSampleRate); // ~2ms

    int lastOnset = rangeStartSample - holdoffSamples;
    int lastFinalOnset = rangeStartSample - 1; // guards monotonic ordering after the walk-back/snap below can shift a crossing earlier than the previous one's

    for (int i = juce::jmax (1, rangeStartSample); i < rangeEndSample; ++i)
    {
        if (onsetDerivative[(size_t) i] > threshold
            && onsetDerivative[(size_t) (i - 1)] <= threshold
            && (i - lastOnset) >= holdoffSamples)
        {
            // Walk backward from the threshold crossing to the actual start
            // of the rise, not the point the rise-rate happened to cross an
            // arbitrary threshold.
            int riseStart = i;
            const int walkBackLimit = juce::jmax (rangeStartSample, i - maxWalkBackSamples);

            while (riseStart > walkBackLimit && onsetEnvelope[(size_t) riseStart] > silenceFloor)
                --riseStart;

            const int snapped = snapToNearestZeroCrossing (riseStart, zeroCrossingSearchRadius);
            const int finalOnset = juce::jlimit (juce::jmax (rangeStartSample, lastFinalOnset + 1),
                                                  rangeEndSample - 1, snapped);

            onsets.push_back (finalOnset);
            lastFinalOnset = finalOnset;
            lastOnset = i; // holdoff is measured from the raw crossing, not the (possibly earlier) walked-back/snapped position
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

    const auto& wholeOnsets = cachedWholeBufferOnsets (sensitivity, holdoffMs, method);

    // Filter the cached whole-buffer onsets to the requested range. This is
    // equivalent to pick*Onsets(sens, holdoff, rangeStart, rangeEnd): holdoff
    // spacing is already enforced whole-buffer, and the first in-range onset
    // is always accepted by range-limited detection regardless of the seed,
    // so no onsets are gained or lost by filtering. (One ultra-edge-case
    // exception: the onset pipeline's zero-crossing snap can push an onset
    // past rangeEnd, which range-limited detection would have clamped into
    // the range — an invisible 1-sample boundary at the very trim edge.)
    std::vector<int> onsets;
    onsets.reserve (wholeOnsets.size() / 2);

    const auto first = std::lower_bound (wholeOnsets.begin(), wholeOnsets.end(), rangeStartSample);
    const auto last = std::upper_bound (first, wholeOnsets.end(), rangeEndSample - 1);
    onsets.insert (onsets.end(), first, last);

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

const std::vector<int>& TransientDetector::cachedWholeBufferOnsets (float sensitivity, float holdoffMs, DetectionMethod method) const
{
    auto& cache = onsetCache[(size_t) method];

    const bool valid = cache.valid
        && cache.sensitivity == sensitivity
        && cache.holdoffMs == holdoffMs
        && cache.numSamplesCached == numSamples;

    if (valid)
        return cache.onsets;

    cache.valid = true;
    cache.sensitivity = sensitivity;
    cache.holdoffMs = holdoffMs;
    cache.numSamplesCached = numSamples;
    cache.onsets = (method == DetectionMethod::onset)
                       ? pickOnsetOnsets (sensitivity, holdoffMs, 0, numSamples)
                       : pickPeakOnsets (sensitivity, holdoffMs, 0, numSamples);

    return cache.onsets;
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
