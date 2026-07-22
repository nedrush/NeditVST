#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <array>
#include <cmath>
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

    // Note-value palette shared by the clock-reference menu and the
    // subdivision probability table (Step 14/15). Matches the standard
    // Max/M4L tempo-relative rate set, sorted shortest to longest, capped
    // at 1n (1nd — 1.5 bars — deliberately excluded per the "no longer
    // than 1 bar" decision). Beats are quarter-note units.
    struct NoteValueOption { const char* name; double beats; };

    const std::array<NoteValueOption, SlicerAudioProcessor::numNoteValueOptions> noteValueOptions { {
        { "128n", 1.0 / 32.0 },
        { "64n",  1.0 / 16.0 },
        { "32nt", 1.0 / 12.0 },
        { "64nd", 3.0 / 32.0 },
        { "32n",  1.0 / 8.0 },
        { "16nt", 1.0 / 6.0 },
        { "32nd", 3.0 / 16.0 },
        { "16n",  1.0 / 4.0 },
        { "8nt",  1.0 / 3.0 },
        { "16nd", 3.0 / 8.0 },
        { "8n",   1.0 / 2.0 },
        { "4nt",  2.0 / 3.0 },
        { "8nd",  3.0 / 4.0 },
        { "4n",   1.0 },
        { "2nt",  4.0 / 3.0 },
        { "4nd",  3.0 / 2.0 },
        { "2n",   2.0 },
        { "1nt",  8.0 / 3.0 },
        { "2nd",  3.0 },
        { "1n",   4.0 }
    } };

    // Playback style names (Step 19/21/22), indexed the same way the
    // weighted table stores them.
    const std::array<const char*, SlicerAudioProcessor::numPlaybackStyleOptions> playbackStyleNames { {
        "Forward", "Ping-Pong", "Tape Stop", "Stretch"
    } };

    // Tape Stop scope names (Step 21).
    const std::array<const char*, SlicerAudioProcessor::numTapeStopScopeOptions> tapeStopScopeNames { {
        "Whole window", "Per tick"
    } };
}

juce::String SlicerAudioProcessor::getPlaybackStyleName (int index)
{
    if (index < 0 || index >= numPlaybackStyleOptions)
        return {};

    return playbackStyleNames[(size_t) index];
}

juce::String SlicerAudioProcessor::getTapeStopScopeName (int index)
{
    if (index < 0 || index >= numTapeStopScopeOptions)
        return {};

    return tapeStopScopeNames[(size_t) index];
}

juce::String SlicerAudioProcessor::getNoteValueName (int index)
{
    if (index < 0 || index >= numNoteValueOptions)
        return {};

    return noteValueOptions[(size_t) index].name;
}

double SlicerAudioProcessor::getNoteValueBeats (int index)
{
    if (index < 0 || index >= numNoteValueOptions)
        return 1.0;

    return noteValueOptions[(size_t) index].beats;
}

int SlicerAudioProcessor::nearestNoteValueIndex (double targetBeats)
{
    int bestIndex = 0;
    double bestDistance = std::numeric_limits<double>::max();

    for (int i = 0; i < numNoteValueOptions; ++i)
    {
        const double distance = std::abs (getNoteValueBeats (i) - targetBeats);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

//==============================================================================
SlicerAudioProcessor::SlicerAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    formatManager.registerBasicFormats();
    subdivisionProbabilities.assign (numNoteValueOptions, 1.0f);

    // Forward-only by default (NOT even odds like the other tables) --
    // guarantees byte-identical default playback, since none of Ping-Pong/
    // Tape Stop/Stretch is ever drawn unless the user explicitly turns its
    // weight up. (Step 22's spec described this as "all other styles at
    // weight 1, Stretch at weight 0," which would actually break that
    // guarantee for existing users -- kept Forward-only here instead,
    // since "must sound identical to current behavior" is the
    // longstanding hard requirement across every style added so far.)
    playbackStyleProbabilities = { 1.0f, 0.0f, 0.0f, 0.0f };
}

SlicerAudioProcessor::~SlicerAudioProcessor() = default;

void SlicerAudioProcessor::prepareToPlay (double /*sampleRate*/, int /*samplesPerBlock*/)
{
    hasCurrentPick = false;
    clockModeInitialized = false;
    clockCurrentPickValid = false;
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
        clockModeInitialized = false;
        clockCurrentPickValid = false;
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
    // faster than host (slow down, pitch drops). Applies in both trigger
    // modes — it's purely a playback-speed/pitch thing, independent of
    // when triggers happen.
    const double loopLengthQuarterNotes = (double) loopLengthBars.load() * 4.0; // assumes 4/4
    const double sourceSpanSeconds = computeSourceSpanSeconds();
    const double hostLoopLengthSeconds = loopLengthQuarterNotes * (60.0 / hostBpm);
    const double repitchRatio = (hostLoopLengthSeconds > 0.0)
                                     ? (sourceSpanSeconds / hostLoopLengthSeconds)
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

    const bool clockMode = (triggerMode.load() == TriggerMode::clock);

    // Time-Stretch (Step 17): grain scheduling derived from the same
    // playbackRate math above — a grain spawns every outputHopSamples
    // (host domain, half the grain length for the fixed 50% overlap),
    // sourceHopSamples further into the source than the last one, which
    // works out to exactly the same average source-consumption rate as
    // playbackRate. Each grain itself only gets srConversionRatio applied
    // (sample-rate matching, not a pitch effect) — never repitchRatio —
    // which is what keeps pitch fixed regardless of tempo.
    const bool timeStretchMode = (pitchMode.load() == PitchMode::timeStretch);
    const double srConversionRatio = sampleSampleRate / hostSampleRate;
    const double grainSizeHostSamples = (double) grainSizeMs.load() / 1000.0 * hostSampleRate;
    const double outputHopSamples = grainSizeHostSamples * 0.5; // fixed 50% overlap, not exposed
    const double sourceHopSamples = outputHopSamples * srConversionRatio * repitchRatio;
    const GranularStretcher::WindowShape grainWindowShapeForBlock =
        (grainWindowShape.load() == GrainWindowShape::hann) ? GranularStretcher::WindowShape::hann
                                                              : GranularStretcher::WindowShape::triangular;

    // Pitch control (Step 18): scales only each grain's own internal
    // read-rate (below, in renderAndAdvance) — never outputHopSamples or
    // sourceHopSamples above, which is what keeps stretch amount and
    // pitch independently controllable. 0 semitones -> pitchRatio == 1.0,
    // a complete no-op, same as before this existed.
    const double pitchRatio = std::pow (2.0, (double) pitchShiftSemitones.load() / 12.0);

    if (granularNeedsReseed.exchange (false))
        granularStretcher.reset (currentPosition); // pitch mode changed mid-pick — reseed from wherever we are now

    // Only Clock mode needs the host's beat position — Slice Length mode
    // paces itself purely from slice content length and never looks at ppq.
    double ppqStart = 0.0;
    double ppqPerSample = 0.0;

    if (clockMode)
    {
        ppqStart = position->getPpqPosition().hasValue() ? *position->getPpqPosition() : 0.0;
        ppqPerSample = (hostBpm / 60.0) / hostSampleRate;

        if (! clockModeInitialized)
        {
            // Just entered Clock mode, or transport just started — snap to
            // the window we're currently inside and force an immediate
            // pick on the very first sample below.
            const double windowBeats = getNoteValueBeats (clockReferenceIndex.load());
            const juce::int64 windowIndex = (juce::int64) std::floor (ppqStart / windowBeats);
            windowEndPpq = (double) (windowIndex + 1) * windowBeats;
            nextTickPpq = ppqStart;
            clockModeInitialized = true;
        }
    }
    else
    {
        clockModeInitialized = false; // so re-entering Clock mode later starts fresh
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // Set true whenever a fresh pick begins this sample (new slice
        // chosen, or a Clock-mode retrigger) — the cue to reseed the
        // granular engine so it starts a new grain right at the pick's
        // start rather than mid-grain from whatever the last pick left it
        // doing. Reset unconditionally, regardless of pitch mode, so it's
        // already in sync if the user switches mode later mid-stream.
        bool pickJustStarted = false;

        if (clockMode)
        {
            const double samplePpq = ppqStart + (double) i * ppqPerSample;

            if (samplePpq >= nextTickPpq)
            {
                const bool newWindow = ! clockCurrentPickValid || samplePpq >= windowEndPpq;

                if (newWindow)
                {
                    clockCurrentSliceIndex = pickWeightedRandomSlice();
                    clockCurrentSubdivisionIndex = pickWeightedIndex (subdivisionProbabilities);
                    clockCurrentPlaybackStyle = indexToPlaybackStyle (pickWeightedIndex (playbackStyleProbabilities));
                    clockCurrentPickValid = true;

                    const double windowBeats = getNoteValueBeats (clockReferenceIndex.load());
                    const juce::int64 windowIndex = (juce::int64) std::floor (samplePpq / windowBeats);
                    windowEndPpq = (double) (windowIndex + 1) * windowBeats;
                }

                // Retrigger (or first-trigger) this window's slice from its
                // start — unconditionally, even if it hadn't naturally
                // finished yet. That's the whole point of Clock mode. Every
                // tick restarts the round trip from the beginning (forward
                // leg) even within the same window, same as Forward always
                // restarted from the slice's start on every tick.
                if (clockCurrentSliceIndex >= 0 && clockCurrentSliceIndex < (int) slices.size())
                {
                    const auto& slice = slices[(size_t) clockCurrentSliceIndex];
                    const bool pingPong = (clockCurrentPlaybackStyle == PlaybackStyle::pingPong);
                    const bool tapeStop = (clockCurrentPlaybackStyle == PlaybackStyle::tapeStop);
                    const bool stretch = (clockCurrentPlaybackStyle == PlaybackStyle::stretch);

                    currentPlaybackStyle = clockCurrentPlaybackStyle;
                    currentSliceStartSample = slice.startSample;
                    currentSliceLength = slice.endSample - slice.startSample;
                    currentPosition = (double) slice.startSample;
                    currentEndSample = pingPong ? (2 * slice.endSample - slice.startSample)
                                     : stretch ? (int) (slice.startSample + stretchDurationMultiplier * currentSliceLength)
                                               : slice.endSample;
                    hasCurrentPick = true;
                    pickJustStarted = true;
                    currentlyPlayingSliceIndexForUI.store (clockCurrentSliceIndex);

                    // Beat-quantized slice length (Step 24) never applies in
                    // Clock mode -- its own tick system already enforces
                    // beat-alignment.
                    currentPickBeatQuantized = false;

                    samplesSincePickStart = 0.0;
                    const double naturalLengthHostSamples =
                        (playbackRate > 0.0) ? ((double) currentSliceLength / playbackRate) : 0.0;

                    // Where a Ping-Pong round trip reverses direction —
                    // always one slice's worth of natural playback time,
                    // regardless of whether the tick below ends up cutting
                    // the pick off before ever reaching it. Unused for
                    // Forward.
                    currentPickMidpointHostSamples = naturalLengthHostSamples;

                    const double roundTripLengthHostSamples = pingPong ? (2.0 * naturalLengthHostSamples) : naturalLengthHostSamples;

                    const double tickBeats = getNoteValueBeats (clockCurrentSubdivisionIndex);
                    const double tickLengthHostSamples = tickBeats * (60.0 / hostBpm) * hostSampleRate;

                    // Shared by Tape Stop's whole-window scope and Stretch
                    // (which always behaves like whole-window) below.
                    const double windowBeatsForDuration = getNoteValueBeats (clockReferenceIndex.load());
                    const double windowLengthHostSamples = windowBeatsForDuration * (60.0 / hostBpm) * hostSampleRate;

                    if (tapeStop)
                    {
                        // Tape Stop's duration is entirely scope-driven —
                        // NOT capped by the slice's own natural length like
                        // Forward/Ping-Pong deliberately are just below,
                        // since the whole point is that read position may
                        // never reach the slice's actual end before the
                        // rate hits zero.
                        const bool wholeWindow = (tapeStopScope.load() == TapeStopScope::wholeWindow);
                        currentPickTapeStopDurationHostSamples = wholeWindow ? windowLengthHostSamples : tickLengthHostSamples;
                        currentPickLengthInHostSamples = currentPickTapeStopDurationHostSamples; // only used for fadeIn clamping below
                    }
                    else if (stretch)
                    {
                        // Stretch always overrides the whole window (no
                        // per-tick option) -- capped by whichever comes
                        // first: the full stretchDurationMultiplier-x
                        // natural length, or the window's own boundary
                        // (mirrors Forward/Ping-Pong's own "whichever comes
                        // first" clamp against a tick, just against the
                        // window instead, since there's no tick to speak of
                        // here).
                        currentPickLengthInHostSamples = juce::jmin (stretchDurationMultiplier * naturalLengthHostSamples,
                                                                      windowLengthHostSamples);
                    }
                    else
                    {
                        // The fade-out needs to anticipate whichever comes
                        // first — the slice's own natural (round-trip, for
                        // Ping-Pong) end, or the forced retrigger at the next
                        // tick — otherwise a slice that gets cut short by the
                        // clock never gets a fade-out at all, and every
                        // retrigger clicks.
                        currentPickLengthInHostSamples = juce::jmin (roundTripLengthHostSamples, tickLengthHostSamples);
                    }
                }
                else
                {
                    hasCurrentPick = false;
                }

                if ((clockCurrentPlaybackStyle == PlaybackStyle::tapeStop
                     && tapeStopScope.load() == TapeStopScope::wholeWindow)
                    || clockCurrentPlaybackStyle == PlaybackStyle::stretch)
                {
                    // Whole-window Tape Stop, and Stretch (which always
                    // behaves this way, no per-tick option), override
                    // normal subdivision retriggering -- one continuous
                    // render spans the entire window, so there's nothing
                    // for a tick to do until the window itself changes.
                    // Jumping straight to the window's end means the next
                    // event that fires IS that boundary, which is
                    // naturally a fresh newWindow pick — no separate
                    // retrigger-skipping logic needed above.
                    nextTickPpq = windowEndPpq;
                }
                else
                {
                    const double subdivisionBeats = getNoteValueBeats (clockCurrentSubdivisionIndex);
                    nextTickPpq += juce::jmax (subdivisionBeats, 1.0e-6); // guard against a zero-length tick
                    nextTickPpq = juce::jmin (nextTickPpq, windowEndPpq);
                }
            }
        }
        else
        {
            // Slice Length mode (unchanged): pick a fresh slice whenever
            // nothing's playing or the current one has run its course.
            // Tape Stop's "run its course" is time-based (its read
            // position deliberately never reaches currentEndSample -- see
            // currentPickTapeStopDurationHostSamples), unlike Forward/
            // Ping-Pong which are always position-based.
            int pickAttempts = 0;

            while (! hasCurrentPick
                   || (currentPlaybackStyle == PlaybackStyle::tapeStop
                           ? samplesSincePickStart >= currentPickTapeStopDurationHostSamples
                           : currentPosition >= (double) (((currentPlaybackStyle == PlaybackStyle::pingPong
                                                                 || currentPlaybackStyle == PlaybackStyle::stretch)
                                                                ? currentEndSample
                                                                : juce::jmin (currentEndSample, sourceLength)) - 1)))
            {
                currentSliceIndex = pickWeightedRandomSlice();

                if (currentSliceIndex < 0 || currentSliceIndex >= (int) slices.size())
                {
                    hasCurrentPick = false;
                    break;
                }

                currentPlaybackStyle = indexToPlaybackStyle (pickWeightedIndex (playbackStyleProbabilities));
                const bool pingPong = (currentPlaybackStyle == PlaybackStyle::pingPong);
                const bool stretch = (currentPlaybackStyle == PlaybackStyle::stretch);

                const auto& slice = slices[(size_t) currentSliceIndex];
                currentSliceStartSample = slice.startSample;
                currentSliceLength = slice.endSample - slice.startSample;
                currentPosition = (double) slice.startSample;
                currentEndSample = pingPong ? (2 * slice.endSample - slice.startSample)
                                 : stretch ? (int) (slice.startSample + stretchDurationMultiplier * currentSliceLength)
                                           : slice.endSample;
                hasCurrentPick = true;
                pickJustStarted = true;
                currentlyPlayingSliceIndexForUI.store (currentSliceIndex);

                samplesSincePickStart = 0.0;
                const double naturalLengthHostSamples = (playbackRate > 0.0) ? ((double) currentSliceLength / playbackRate) : 0.0;
                currentPickMidpointHostSamples = naturalLengthHostSamples; // where a Ping-Pong round trip reverses; unused for Forward
                currentPickTapeStopDurationHostSamples = naturalLengthHostSamples; // the pick's own natural length; unused for Forward/Ping-Pong/Stretch
                currentPickLengthInHostSamples = pingPong ? (2.0 * naturalLengthHostSamples)
                                                : stretch ? (stretchDurationMultiplier * naturalLengthHostSamples)
                                                          : naturalLengthHostSamples;

                // Beat-quantized slice length (Step 24) — Time-Stretch +
                // Slice Length mode only, Forward/Ping-Pong only (Tape Stop
                // and Stretch already deliberately override natural
                // duration, so quantizing would fight rather than serve
                // them). Computed once here, at pick-start, and consulted
                // by the render step below for the rest of this pick's life.
                currentPickBeatQuantized = false;

                if (timeStretchMode && beatQuantizeSliceLengthEnabled.load()
                    && (currentPlaybackStyle == PlaybackStyle::forward || pingPong)
                    && currentSliceLength > 0)
                {
                    const double originalBpm = getCalculatedOriginalBpm();

                    if (originalBpm > 0.0)
                    {
                        // Ping-Pong quantizes the FULL ROUND TRIP (there and
                        // back) as one unit, before the fold -- same
                        // "whichever unit should land on the beat grid" span
                        // currentPickLengthInHostSamples already uses above.
                        const double sliceNaturalSourceSeconds =
                            (double) (pingPong ? 2 * currentSliceLength : currentSliceLength) / sampleSampleRate;
                        const double naturalBeats = sliceNaturalSourceSeconds * originalBpm / 60.0;
                        const double quantizedBeats = getNoteValueBeats (nearestNoteValueIndex (naturalBeats));
                        const double targetHostSeconds = quantizedBeats * (60.0 / hostBpm);

                        if (targetHostSeconds > 0.0)
                        {
                            currentPickBeatQuantized = true;
                            currentPickQuantizedStretchRatio = sliceNaturalSourceSeconds / targetHostSeconds;

                            const double quantizedLengthHostSamples = targetHostSeconds * hostSampleRate;
                            currentPickLengthInHostSamples = quantizedLengthHostSamples;

                            if (pingPong)
                                currentPickMidpointHostSamples = quantizedLengthHostSamples * 0.5;
                        }
                    }
                }

                if (++pickAttempts > 1000)
                    return; // safety bail — render the rest of this block as silence
            }
        }

        if (pickJustStarted)
            granularStretcher.reset (currentPosition); // matches whichever mode's active; harmless if unused this pick

        const bool pingPongActive = (currentPlaybackStyle == PlaybackStyle::pingPong);
        const bool tapeStopActive = (currentPlaybackStyle == PlaybackStyle::tapeStop);
        const bool stretchActive = (currentPlaybackStyle == PlaybackStyle::stretch);

        // Ping-Pong's currentEndSample is a full round trip (2x slice
        // length), and Stretch's is stretchDurationMultiplier-x — both can
        // legitimately run past sourceLength when the slice sits at the
        // very end of the buffer. That's fine: the actual read position
        // below is always either the FOLDED one (Ping-Pong) or safely
        // bounded to the true slice by GranularStretcher itself (Stretch
        // always renders through it), regardless. The sourceLength clamp
        // is only needed for Forward/Tape Stop, where the raw (unfolded)
        // position IS the read position.
        const bool extendedRangeActive = pingPongActive || stretchActive;
        const int schedulingEndSample = extendedRangeActive ? currentEndSample : juce::jmin (currentEndSample, sourceLength);

        // Shared render step for both modes: only output a sample while
        // we're within the current pick's bounds. In Clock mode, once a
        // short slice naturally finishes before the next tick, this
        // condition goes false and we correctly render silence until the
        // next forced retrigger resets currentPosition. For Tape Stop,
        // this condition normally stays true for the pick's whole life —
        // by design, position crawls toward but never reaches
        // schedulingEndSample before the rate hits zero (see
        // currentPickTapeStopDurationHostSamples, which is what actually
        // ends the pick in Slice Length mode instead).
        if (hasCurrentPick && currentPosition < (double) (schedulingEndSample - 1))
        {
            // Fade gain: clamp each fade to at most half this pick's own
            // (effective) length, so a very short slice/tick can't have
            // overlapping/inverted fades that would silence it entirely.
            // Shared by both pitch modes — an overall envelope wrapped
            // around whichever render step produced the dry sample below.
            const double halfPickLength = currentPickLengthInHostSamples * 0.5;
            const double fadeInSamples = juce::jmin (fadeInSamplesRequested, halfPickLength);
            const double fadeOutSamples = juce::jmin (fadeOutSamplesRequested, halfPickLength);
            const double samplesRemaining = currentPickLengthInHostSamples - samplesSincePickStart;

            double gain = 1.0;

            if (fadeInSamples > 0.0 && samplesSincePickStart < fadeInSamples)
                gain = samplesSincePickStart / fadeInSamples;

            // Tape Stop (Step 21): an additional rate multiplier, ramping
            // linearly from 1.0 to 0.0 across currentPickTapeStopDuration-
            // HostSamples, layered on top of whatever Pitch Mode already
            // produces below. Gain rides the SAME curve, REPLACING (not
            // stacking with) the normal fadeOutMs for this style — if rate
            // hit exactly 0 while gain stayed at full, the engine would
            // get stuck holding/repeating a single sample (a buzz) instead
            // of fading to silence. fadeInMs above is unaffected.
            double tapeStopRateMultiplier = 1.0;

            if (tapeStopActive)
            {
                const double progress = (currentPickTapeStopDurationHostSamples > 0.0)
                    ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickTapeStopDurationHostSamples)
                    : 1.0; // degenerate zero-length duration -- treat as already fully stopped
                tapeStopRateMultiplier = 1.0 - progress;

                gain = juce::jmin (gain, tapeStopRateMultiplier);
            }
            else
            {
                if (fadeOutSamples > 0.0 && samplesRemaining < fadeOutSamples)
                    gain = juce::jmin (gain, samplesRemaining / fadeOutSamples);

                // Ping-Pong (Step 19): the bounce point isn't a true edit --
                // audio isn't symmetric around any given sample, so
                // reversing direction there clicks just like the pick's own
                // start/end would without a fade. Treat the midpoint as an
                // internal boundary and apply the SAME fadeInMs/fadeOutMs
                // envelope around it: fading out approaching it (mirrors
                // the pick's own end-fade) and back in leaving it (mirrors
                // the start-fade). Layered into the same overall `gain`, so
                // it wraps whichever render path below produced the dry
                // sample -- Repitch or Time-Stretch alike.
                if (pingPongActive)
                {
                    const double distanceBeforeMidpoint = currentPickMidpointHostSamples - samplesSincePickStart;
                    const double distanceAfterMidpoint = samplesSincePickStart - currentPickMidpointHostSamples;

                    if (distanceBeforeMidpoint >= 0.0 && distanceBeforeMidpoint < fadeOutSamples)
                        gain = juce::jmin (gain, distanceBeforeMidpoint / fadeOutSamples);

                    if (distanceAfterMidpoint >= 0.0 && distanceAfterMidpoint < fadeInSamples)
                        gain = juce::jmin (gain, distanceAfterMidpoint / fadeInSamples);
                }
            }

            gain = juce::jlimit (0.0, 1.0, gain);

            // Tape Stop doesn't fold position (it decelerates a plain
            // forward read) -- grainPlaybackStyle naturally comes out as
            // forward for it, same as it already does for anything that
            // isn't Ping-Pong.
            const GranularStretcher::PlaybackStyle grainPlaybackStyle =
                pingPongActive ? GranularStretcher::PlaybackStyle::pingPong : GranularStretcher::PlaybackStyle::forward;

            if (timeStretchMode || stretchActive)
            {
                // Stretch (Step 22) always renders through GranularStretcher,
                // even in Repitch mode -- it's a deliberate character
                // effect, not something that should vanish depending on an
                // unrelated global toggle. It gets its OWN small, hard-
                // edged, fixed grain parameters here (never the user-facing
                // Pitch Mode Time-Stretch grain size/window shape/pitch
                // shift, none of which apply): outputHopSamples/grainSize
                // come from stretchCharacterGrainSizeMs, and sourceHopSamples
                // is derived from playbackRate/stretchDurationMultiplier --
                // consuming the whole slice at a quarter the normal rate is
                // exactly what makes the render last stretchDurationMultiplier
                // times as long. Tape Stop's rate multiplier, by contrast,
                // is layered onto BOTH each grain's own internal read-rate
                // (the same slot pitchRatio already multiplies) AND the
                // source-domain distance between grain spawns -- so as the
                // decel progresses, new grains converge toward the same,
                // increasingly frozen position instead of continuing to
                // introduce fresh material at the original pace while only
                // their internal read slows. outputHopSamples (the
                // real-time spawn cadence) is deliberately left alone in
                // that case, same as pitchRatio never touches it either.
                double grainOutputHopSamples = outputHopSamples;
                // Beat-quantized slice length (Step 24): this pick's own
                // stretch ratio replaces the global repitchRatio here,
                // symmetrically with the scheduling-position advance rate
                // below -- currentPickBeatQuantized is only ever true for
                // Forward/Ping-Pong Slice-Length picks (never alongside
                // tapeStopActive), so these two overrides can't collide.
                double grainSourceHopSamples = tapeStopActive ? (sourceHopSamples * tapeStopRateMultiplier)
                                              : currentPickBeatQuantized ? (outputHopSamples * srConversionRatio * currentPickQuantizedStretchRatio)
                                                                         : sourceHopSamples;
                double grainGrainSizeHostSamples = grainSizeHostSamples;
                double grainPitchRatio = tapeStopActive ? (pitchRatio * tapeStopRateMultiplier) : pitchRatio;
                GranularStretcher::WindowShape grainWindowShapeToUse = grainWindowShapeForBlock;

                if (stretchActive)
                {
                    grainGrainSizeHostSamples = (double) stretchCharacterGrainSizeMs / 1000.0 * hostSampleRate;
                    grainOutputHopSamples = grainGrainSizeHostSamples * 0.5; // same fixed 50% overlap convention
                    grainSourceHopSamples = grainOutputHopSamples * (playbackRate / stretchDurationMultiplier);
                    grainPitchRatio = 1.0; // no user pitch shift for this style -- fully self-contained/hardcoded
                    grainWindowShapeToUse = GranularStretcher::WindowShape::hardEdge;
                }

                float channelSums[GranularStretcher::maxChannels] = {};
                granularStretcher.renderAndAdvance (sampleBuffer, sourceChannels,
                                                     grainOutputHopSamples, grainSourceHopSamples,
                                                     (double) currentSliceStartSample, (double) currentSliceLength, grainPlaybackStyle,
                                                     grainGrainSizeHostSamples, srConversionRatio, grainPitchRatio,
                                                     grainWindowShapeToUse, channelSums);

                for (int outCh = 0; outCh < outChannels; ++outCh)
                {
                    const int srcCh = juce::jmin (juce::jmin (outCh, sourceChannels - 1), GranularStretcher::maxChannels - 1);
                    buffer.addSample (outCh, i, channelSums[srcCh] * (float) gain);
                }
            }
            else
            {
                // Shared position-mapping (Step 19): the same foldPosition()
                // GranularStretcher uses for its own grain-start scheduling,
                // so Ping-Pong behaves identically regardless of pitch mode.
                // For Forward this is the identity — foldedReadPosition ==
                // currentPosition exactly, same as before this existed.
                const double foldedReadPosition = (double) currentSliceStartSample
                    + GranularStretcher::foldPosition (currentPosition - (double) currentSliceStartSample,
                                                        (double) currentSliceLength, grainPlaybackStyle);

                const int idx0 = juce::jlimit (0, sourceLength - 1, (int) foldedReadPosition);
                const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
                const float frac = (float) (foldedReadPosition - (double) idx0);

                for (int outCh = 0; outCh < outChannels; ++outCh)
                {
                    const int srcCh = juce::jmin (outCh, sourceChannels - 1);
                    const float s0 = sampleBuffer.getSample (srcCh, idx0);
                    const float s1 = sampleBuffer.getSample (srcCh, idx1);
                    const float sample = (s0 + frac * (s1 - s0)) * (float) gain;

                    buffer.addSample (outCh, i, sample);
                }
            }

            // Tape Stop layers its rate multiplier onto the SAME shared
            // advance both pitch modes' scheduling position relies on --
            // this is what makes the read position (and therefore
            // schedulingEndSample/foldedReadPosition above) actually slow
            // down and, as the spec calls out, deliberately fall short of
            // the slice's real end sample in real time. Beat-quantized
            // slice length (Step 24) substitutes this pick's own quantized
            // stretch ratio for repitchRatio here too, in exactly the same
            // place playbackRate itself is built from repitchRatio above --
            // that's what keeps currentPosition (and therefore this pick's
            // actual finish time) landing precisely on the quantized target
            // duration, matching the granular hop schedule above sample for
            // sample.
            const double effectivePlaybackRate = tapeStopActive ? (playbackRate * tapeStopRateMultiplier)
                                                : currentPickBeatQuantized ? ((sampleSampleRate / hostSampleRate) * currentPickQuantizedStretchRatio)
                                                                           : playbackRate;
            currentPosition += effectivePlaybackRate;
            samplesSincePickStart += 1.0;
        }
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

        // Trim markers (Step 23): default to the full sample length, so
        // behaviour is unchanged until the user actually drags a handle.
        trimStartSample.store (0);
        trimEndSample.store (sampleBuffer.getNumSamples());

        manualPoints.clear(); // positions from the old sample don't mean anything here
        excludedPoints.clear();
        hasCurrentPick = false; // don't let a stale pick read past the new buffer's end
        clockModeInitialized = false;
        clockCurrentPickValid = false;
    }

    undoManager.clearUndoHistory(); // old undo steps reference positions in a different file now

    redetectSlices (defaultSensitivity, defaultHoldoffMs);
}

void SlicerAudioProcessor::redetectSlices (float sensitivity, float holdoffMs)
{
    rebuildSlicesFromDetectionAndManualPoints (sensitivity, holdoffMs);
}

double SlicerAudioProcessor::computeSourceSpanSeconds() const
{
    if (manualBpmOverrideEnabled.load())
    {
        const double bpm = manualBpmOverrideValue.load();

        if (bpm <= 0.0)
            return 0.0;

        const double beats = (double) loopLengthBars.load() * 4.0; // assumes 4/4, same as elsewhere
        return (beats * 60.0) / bpm;
    }

    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    const int spanSamples = juce::jmax (0, trimEnd - trimStart);
    return (double) spanSamples / sampleSampleRate;
}

void SlicerAudioProcessor::rebuildSlicesFromDetectionAndManualPoints (float sensitivity, float holdoffMs)
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    auto autoSlices = transientDetector.detectSlices (sensitivity, holdoffMs, trimStart, trimEnd);

    const juce::ScopedLock sl (sampleLock);

    slices = mergeOnsetsIntoSlices (autoSlices, trimStart, trimEnd);
    sliceProbabilities.assign (slices.size(), 1.0f); // default: even odds across all slices
    hasCurrentPick = false; // boundaries changed — force a fresh pick
    clockModeInitialized = false;
    clockCurrentPickValid = false;
}

std::vector<Slice> SlicerAudioProcessor::previewSlicesAtSensitivity (float sensitivity) const
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    auto autoSlices = transientDetector.detectSlices (sensitivity, defaultHoldoffMs, trimStart, trimEnd);

    const juce::ScopedLock sl (sampleLock);
    return mergeOnsetsIntoSlices (autoSlices, trimStart, trimEnd);
}

std::vector<Slice> SlicerAudioProcessor::mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices, int trimStart, int trimEnd) const
{
    const int matchToleranceSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);

    std::vector<int> onsets;
    onsets.reserve (autoSlices.size() + manualPoints.size());

    for (const auto& s : autoSlices)
    {
        if (s.startSample == trimStart)
        {
            onsets.push_back (trimStart); // the trim start is never excludable
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

    // Manual points outside the current trim range are filtered out here
    // (not deleted from manualPoints itself) — the same "soft exclude"
    // widening the trim back out later can undo, matching excludedPoints'
    // own semantics just above.
    for (const auto& mp : manualPoints)
        if (mp.samplePosition > trimStart && mp.samplePosition < trimEnd)
            onsets.push_back (mp.samplePosition);

    if (onsets.empty() || onsets.front() != trimStart)
        onsets.insert (onsets.begin(), trimStart);

    std::sort (onsets.begin(), onsets.end());
    onsets.erase (std::unique (onsets.begin(), onsets.end()), onsets.end());

    std::vector<Slice> result;

    for (size_t i = 0; i < onsets.size(); ++i)
    {
        Slice slice;
        slice.startSample = onsets[i];
        slice.endSample = (i + 1 < onsets.size()) ? onsets[i + 1] : trimEnd;

        if (slice.lengthInSamples() > 0)
            result.push_back (slice);
    }

    return result;
}

int SlicerAudioProcessor::addManualSlicePoint (int targetSample, bool snapToTransient)
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    int snapped = juce::jlimit (trimStart, juce::jmax (trimStart, trimEnd - 1), targetSample);

    if (snapToTransient)
    {
        const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
        snapped = transientDetector.findNearestPeak (snapped, radiusSamples, trimStart, trimEnd);
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
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    int snapped = juce::jlimit (trimStart, juce::jmax (trimStart, trimEnd - 1), targetSample);

    if (snapToTransient)
    {
        const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
        snapped = transientDetector.findNearestPeak (snapped, radiusSamples, trimStart, trimEnd);
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
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();

    // Search the raw current auto-detection result (not the merged
    // `slices`) for the nearest boundary to targetSample — the trim start
    // is never a candidate, it can't be excluded.
    auto autoSlices = transientDetector.detectSlices (currentSensitivity.load(), defaultHoldoffMs, trimStart, trimEnd);

    int nearest = -1;
    int bestDistance = std::numeric_limits<int>::max();

    for (const auto& s : autoSlices)
    {
        if (s.startSample == trimStart)
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
