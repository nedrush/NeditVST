#pragma once

#include <JuceHeader.h>
#include <array>

//==============================================================================
/** Step-17: lightweight overlap-add granular time-stretcher.

    Repitch mode (unchanged, elsewhere) resamples a single read pointer
    through the source at `playbackRate`, so pitch follows playback speed.
    This class is the alternative: it plays short, windowed "grains" of the
    source at their native, sample-rate-corrected-only rate (pitch stays
    fixed relative to the source), while spacing the grains' START
    positions apart to track tempo instead — classic overlap-add granular
    synthesis, no FFT/phase vocoder. An optional pitchRatio (Step 18) can
    then shift that fixed pitch up or down without touching how the grains
    are scheduled, so stretch amount and pitch stay independent.

    Owns no reference to the source buffer or to any scheduling state
    (which slice is picked, when the next one gets triggered) — that stays
    entirely the processor's job and is identical for both pitch modes.
    This only turns "the pick currently in progress, from this source
    position" into one host output sample at a time. */
class GranularStretcher
{
public:
    enum class WindowShape { hann, triangular };

    static constexpr int maxChannels = 2;

    // Call whenever a new pick begins (fresh slice chosen, or a Clock-mode
    // retrigger) — clears every grain and queues one to spawn immediately
    // at startSourcePosition, so sound starts with no gap.
    void reset (double startSourcePosition);

    // Call once per host output sample while a pick is active and Time-
    // Stretch mode is selected. Spawns new grains on schedule (every
    // outputHopSamples, sourceHopSamples further into the source than the
    // previous grain's start), advances every active grain by
    // srConversionRatio * pitchRatio, and adds this sample's summed
    // windowed output into channelSumsOut[0 .. sourceChannels-1]
    // (sourceChannels clamped to maxChannels; caller must clear/own that
    // buffer's lifetime).
    //
    // pitchRatio (Step 18) only scales each grain's own internal
    // read-rate — it has no effect on outputHopSamples/sourceHopSamples,
    // which is what keeps stretch amount and pitch independently
    // controllable. pitchRatio == 1.0 is a complete no-op (matches
    // pre-Step-18 behaviour exactly).
    void renderAndAdvance (const juce::AudioBuffer<float>& sourceBuffer,
                            int sourceChannels,
                            double outputHopSamples,
                            double sourceHopSamples,
                            double grainSizeHostSamples,
                            double srConversionRatio,
                            double pitchRatio,
                            WindowShape windowShape,
                            float* channelSumsOut);

private:
    struct Grain
    {
        bool active = false;
        double sourcePosition = 0.0;
        double hostSamplesPlayed = 0.0; // drives both the window envelope and this grain's lifetime
    };

    void spawnGrain (double startSourcePosition);
    static float windowGain (double progress, WindowShape shape);

    // ~2 grains are ever concurrently active at a fixed 50% overlap; this
    // gives headroom rather than cutting it exactly to the minimum.
    static constexpr int numGrains = 4;
    std::array<Grain, numGrains> grains;

    double hopAccumulator = 0.0;
    double nextGrainSourceStart = 0.0;
    bool pendingImmediateSpawn = true;
};
