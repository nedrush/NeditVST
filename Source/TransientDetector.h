#pragma once

#include <JuceHeader.h>
#include <juce_dsp/juce_dsp.h>
#include <complex>
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

//==============================================================================
// TEMPORARY (Onset vs. Peak detection comparison tool): lets detectSlices()
// run either the original peak-picking pipeline (`peak`, threshold-and-hold
// on the derivative of a slow-attack envelope — everything described in the
// file header above) or a new onset pipeline (`onset`) that instead looks
// for where amplitude first starts rising out of its local trough, using a
// much faster envelope/derivative pair so it isn't blurred by the slow
// envelope's own attack smoothing, then snaps that point to the nearest
// zero-crossing for a click-free cut. Both run on every analyze() call so
// the UI can show both marker sets at once for comparison; the toggle in
// the editor just decides which one is passed to detectSlices() for the
// slices that actually get played.
//
// This whole enum, the `method` parameter below, and every onset-only
// member/helper in this class are here ONLY to make that comparison
// possible. Once the decision is made: if Peak wins, delete DetectionMethod,
// the method parameter (detectSlices always behaves as `peak` did), and
// every onset-only member below; if Onset wins, do the same in reverse —
// delete the peak pipeline and make onset's the only path, no toggle.
enum class DetectionMethod
{
    peak,
    onset
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
        whole analysed buffer," matching pre-trim behaviour exactly.
        method (TEMPORARY — see DetectionMethod above): which detection
        pipeline to run. Defaults to `peak`, i.e. every pre-existing call
        site is unaffected unless it explicitly opts into `onset`. */
    std::vector<Slice> detectSlices (float sensitivity, float holdoffMs,
                                      int rangeStartSample = -1, int rangeEndSample = -1,
                                      DetectionMethod method = DetectionMethod::peak) const;

    bool hasAnalysis() const { return ! derivative.empty(); }
    int getAnalyzedLengthInSamples() const { return numSamples; }

    // TEMPORARY (debug overlays for the onset-vs-peak comparison tool):
    // read-only access to the cached envelope-follower outputs so the
    // waveform can plot them. `envelope` is the slow (1ms attack / 50ms
    // release) peak-pipeline follower; `onsetEnvelope` is the fast (0.2ms
    // attack / 2ms release) one. Delete alongside the onset pipeline.
    const std::vector<float>& getPeakEnvelope() const { return envelope; }
    const std::vector<float>& getOnsetEnvelope() const { return onsetEnvelope; }

    // TEMPORARY (spectral debug layer): per-FRAME spectral flux / centroid.
    // sampleToFrame() maps a sample index into the (coarser) frame index so
    // the display can plot these alongside the per-sample envelopes.
    int getSpectralHopSamples() const { return fftHopSamples; }
    int getNumSpectralFrames() const { return (int) spectralFlux.size(); }
    const std::vector<float>& getSpectralFlux() const { return spectralFlux; }
    const std::vector<float>& getSpectralCentroid() const { return spectralCentroid; }

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
    // Peak-picking pipeline (unchanged from the original implementation).
    std::vector<int> pickPeakOnsets (float sensitivity, float holdoffMs,
                                      int rangeStartSample, int rangeEndSample) const;

    // TEMPORARY (onset pipeline — see DetectionMethod above).
    std::vector<int> pickOnsetOnsets (float sensitivity, float holdoffMs,
                                       int rangeStartSample, int rangeEndSample) const;

    // TEMPORARY: searches +/- searchRadiusSamples of index in the raw
    // signed signal for the nearest sign change, returning index unchanged
    // if none is found in range. Used to snap a detected onset to a
    // click-free cut point rather than leaving it wherever the rise-rate
    // threshold happened to walk back to.
    int snapToNearestZeroCrossing (int index, int searchRadiusSamples) const;

    std::vector<float> envelope;
    std::vector<float> derivative;
    double analyzedSampleRate = 44100.0;
    int numSamples = 0;
    float globalMaxDerivative = 0.0f;
    float noiseFloor = 0.0f;

    // TEMPORARY (onset pipeline): raw signed mono-summed signal (no
    // rectification, no envelope smoothing) — needed for zero-crossing
    // snapping, which only makes sense against the actual waveform, not an
    // envelope. A fast-attack/fast-release envelope + its derivative, in
    // the same shape as `envelope`/`derivative` above but with far shorter
    // time constants (see onsetAttackTimeMs/onsetReleaseTimeMs in the .cpp)
    // so it reacts to a rise within a fraction of a millisecond instead of
    // smoothing across ~1ms.
    std::vector<float> signedMonoSignal;
    std::vector<float> onsetEnvelope;

    // TEMPORARY (onset pipeline — spectral debug layer): per-FRAME spectral
    // flux and centroid (one entry per FFT hop, NOT per sample). flux = L2 norm
    // of the positive magnitude change between adjacent frames — the classic
    // broadband-onset cue, structurally immune to the low-fundamental ripple
    // that fooled the fast-envelope derivative. centroid = magnitude-weighted
    // mean bin — a coarse "broadband vs. tonal" cue. The fast-envelope
    // derivative and its global stats were removed since the hybrid detector
    // thresholds the SLOW pipeline's derivative for "that a hit happened".
    // Delete with the onset pipeline once the right feature is validated.
    static constexpr int fftSize = 1024;
    static constexpr int fftHopSamples = 512;
    int spectralBinCount = fftSize / 2 + 1;
    std::vector<float> spectralFlux;
    std::vector<float> spectralCentroid;
    juce::dsp::FFT fft { juce::jmin (32, juce::roundToInt (std::log2 ((float) fftSize))) };
    std::vector<std::complex<float>> fftDataRead;
    std::vector<std::complex<float>> fftDataWrite;
};
