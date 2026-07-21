#include "GranularStretcher.h"
#include <cmath>

double GranularStretcher::foldPosition (double elapsedSourceSamples, double sliceLength, PlaybackStyle style)
{
    if (style == PlaybackStyle::forward || sliceLength <= 0.0)
        return elapsedSourceSamples;

    const double period = 2.0 * sliceLength;
    double cycle = std::fmod (elapsedSourceSamples, period);

    if (cycle < 0.0) // defensive -- elapsedSourceSamples should never go negative in practice
        cycle += period;

    return (cycle < sliceLength) ? cycle : (period - cycle);
}

void GranularStretcher::reset (double startSourcePosition)
{
    for (auto& g : grains)
        g.active = false;

    nextGrainSourceStart = startSourcePosition;
    hopAccumulator = 0.0;
    pendingImmediateSpawn = true;
}

void GranularStretcher::spawnGrain (double startSourcePosition)
{
    Grain* target = nullptr;

    for (auto& g : grains)
    {
        if (! g.active)
        {
            target = &g;
            break;
        }
    }

    // Pool exhausted (shouldn't happen at 50% overlap with 4 slots, but
    // grain size can change live) — steal whichever grain is furthest
    // into its life, since it's already faded closest to silence.
    if (target == nullptr)
    {
        for (auto& g : grains)
            if (target == nullptr || g.hostSamplesPlayed > target->hostSamplesPlayed)
                target = &g;
    }

    target->active = true;
    target->sourcePosition = startSourcePosition;
    target->hostSamplesPlayed = 0.0;
}

float GranularStretcher::windowGain (double progress, WindowShape shape)
{
    progress = juce::jlimit (0.0, 1.0, progress);

    if (shape == WindowShape::hann)
        return (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * progress));

    // Triangular: linear ramp up to the midpoint, then back down.
    return (float) (progress < 0.5 ? (2.0 * progress) : (2.0 * (1.0 - progress)));
}

void GranularStretcher::renderAndAdvance (const juce::AudioBuffer<float>& sourceBuffer,
                                           int sourceChannels,
                                           double outputHopSamples,
                                           double sourceHopSamples,
                                           double sliceStartSample,
                                           double sliceLength,
                                           PlaybackStyle style,
                                           double grainSizeHostSamples,
                                           double srConversionRatio,
                                           double pitchRatio,
                                           WindowShape windowShape,
                                           float* channelSumsOut)
{
    sourceChannels = juce::jlimit (0, maxChannels, sourceChannels);

    for (int ch = 0; ch < sourceChannels; ++ch)
        channelSumsOut[ch] = 0.0f;

    // nextGrainSourceStart marches forward unbounded (same "elapsed since
    // slice start, unfolded" quantity PluginProcessor's currentPosition
    // tracks for its own render path) -- foldPosition() is applied only at
    // the moment a grain actually spawns, so each grain's OWN read still
    // runs forward at its native rate below; only where consecutive grains
    // START bounces back and forth for Ping-Pong.
    const auto spawnAtCurrentMarch = [&]
    {
        const double folded = sliceStartSample + foldPosition (nextGrainSourceStart - sliceStartSample, sliceLength, style);
        spawnGrain (folded);
        nextGrainSourceStart += sourceHopSamples;
    };

    if (pendingImmediateSpawn)
    {
        spawnAtCurrentMarch();
        pendingImmediateSpawn = false;
    }
    else if (outputHopSamples > 0.0)
    {
        hopAccumulator += 1.0;

        while (hopAccumulator >= outputHopSamples)
        {
            spawnAtCurrentMarch();
            hopAccumulator -= outputHopSamples;
        }
    }

    const int sourceLength = sourceBuffer.getNumSamples();

    if (sourceLength == 0)
        return;

    for (auto& grain : grains)
    {
        if (! grain.active)
            continue;

        const double progress = grainSizeHostSamples > 0.0 ? (grain.hostSamplesPlayed / grainSizeHostSamples) : 1.0;
        const float gain = windowGain (progress, windowShape);

        const int idx0 = juce::jlimit (0, sourceLength - 1, (int) grain.sourcePosition);
        const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
        const float frac = (float) (grain.sourcePosition - (double) idx0);

        for (int ch = 0; ch < sourceChannels; ++ch)
        {
            const float s0 = sourceBuffer.getSample (ch, idx0);
            const float s1 = sourceBuffer.getSample (ch, idx1);
            channelSumsOut[ch] += (s0 + frac * (s1 - s0)) * gain;
        }

        // pitchRatio only scales this per-grain read-rate — it never
        // touches outputHopSamples/sourceHopSamples (the hop scheduling
        // above), which is what keeps stretch amount and pitch
        // independently controllable. pitchRatio == 1.0 is a no-op.
        grain.sourcePosition += srConversionRatio * pitchRatio;
        grain.hostSamplesPlayed += 1.0;

        if (grain.hostSamplesPlayed >= grainSizeHostSamples)
            grain.active = false;
    }
}
