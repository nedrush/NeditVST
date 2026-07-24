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

    // Playback style names (Step 19/21/22/29/30), indexed the same way the
    // weighted table stores them.
    const std::array<const char*, SlicerAudioProcessor::numPlaybackStyleOptions> playbackStyleNames { {
        "Forward", "Ping-Pong", "Tape Stop", "Stretch", "Filter Down", "Filter Up"
    } };

    // Tape Stop scope names (Step 21).
    const std::array<const char*, SlicerAudioProcessor::numTapeStopScopeOptions> tapeStopScopeNames { {
        "Whole window", "Per tick"
    } };

    // Filter Sweep scope names (Step 30).
    const std::array<const char*, SlicerAudioProcessor::numFilterSweepScopeOptions> filterSweepScopeNames { {
        "Whole window", "Per tick"
    } };

    // Slice Length periodic reset (Step 34) -- names and their underlying
    // bar counts, held as a parallel pair rather than a NoteValueOption-
    // style struct since bar counts (not beats) are the natural unit
    // here, and every other place in this codebase already converts bars
    // to beats via "* 4" (4/4) rather than storing beats directly.
    const std::array<const char*, SlicerAudioProcessor::numResetBarsOptions> resetBarsNames { {
        "1 bar", "2 bars", "4 bars", "8 bars"
    } };
    const std::array<int, SlicerAudioProcessor::numResetBarsOptions> resetBarsValues { { 1, 2, 4, 8 } };
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

juce::String SlicerAudioProcessor::getFilterSweepScopeName (int index)
{
    if (index < 0 || index >= numFilterSweepScopeOptions)
        return {};

    return filterSweepScopeNames[(size_t) index];
}

juce::String SlicerAudioProcessor::getResetBarsName (int index)
{
    if (index < 0 || index >= numResetBarsOptions)
        return {};

    return resetBarsNames[(size_t) index];
}

int SlicerAudioProcessor::getResetBarsValue (int index)
{
    if (index < 0 || index >= numResetBarsOptions)
        return 4; // matches the default index (2 -> 4 bars)

    return resetBarsValues[(size_t) index];
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

SlicerAudioProcessor::BeatQuantizeResult SlicerAudioProcessor::computeBeatQuantizeTarget (
    int sliceLength, bool pingPong, double sampleSampleRate, double originalBpm, double hostBpm)
{
    BeatQuantizeResult result;

    if (sliceLength <= 0 || originalBpm <= 0.0 || hostBpm <= 0.0)
        return result;

    // Ping-Pong quantizes the FULL ROUND TRIP (there and back) as one
    // unit, before the fold -- same "whichever unit should land on the
    // beat grid" span the pick-length calculation elsewhere uses.
    const double sliceNaturalSourceSeconds = (double) (pingPong ? 2 * sliceLength : sliceLength) / sampleSampleRate;
    const double naturalBeats = sliceNaturalSourceSeconds * originalBpm / 60.0;

    // The note-value palette only goes up to 1n (4 beats, one bar) --
    // searching it for anything longer would wrongly clamp every
    // multi-bar-length slice down to a single bar, regardless of how many
    // bars it actually spans. For those, decompose into whole bars plus a
    // sub-bar remainder, and only run the (still fine-grained, unchanged)
    // palette search on the remainder -- rounding the WHOLE length to the
    // nearest bar (an earlier, cruder version of this fix) was accurate
    // only by coincidence for lengths already near a whole-bar boundary,
    // and crushed anything else (e.g. a 1.3-bar slice) down to a flat
    // whole-bar count. The palette search itself remains exactly as
    // before for anything within one bar to start with, where it was
    // never broken.
    double quantizedBeats;

    if (naturalBeats > 4.0)
    {
        const double wholeBars = std::floor (naturalBeats / 4.0);
        const double remainderBeats = naturalBeats - (wholeBars * 4.0);

        // Below half the smallest palette entry (128n, index 0 -- the
        // palette is sorted shortest to longest), treat as no remainder
        // at all -- otherwise a slice that's already almost exactly N
        // whole bars gets a spurious tiny addition tacked on for no
        // audible reason.
        const double smallestPaletteBeats = getNoteValueBeats (0);
        const double quantizedRemainder = (remainderBeats > smallestPaletteBeats * 0.5)
            ? getNoteValueBeats (nearestNoteValueIndex (remainderBeats))
            : 0.0;

        quantizedBeats = (wholeBars * 4.0) + quantizedRemainder;
    }
    else
    {
        quantizedBeats = getNoteValueBeats (nearestNoteValueIndex (naturalBeats));
    }

    const double targetHostSeconds = quantizedBeats * (60.0 / hostBpm);

    if (targetHostSeconds <= 0.0)
        return result;

    result.quantized = true;
    result.stretchRatio = sliceNaturalSourceSeconds / targetHostSeconds;
    result.targetHostSeconds = targetHostSeconds;
    return result;
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
    // Tape Stop/Stretch/Filter Down/Filter Up is ever drawn unless the
    // user explicitly turns its weight up. (Step 22's spec described this
    // as "all other styles at weight 1, Stretch at weight 0," which would
    // actually break that guarantee for existing users -- kept
    // Forward-only here instead, since "must sound identical to current
    // behavior" is the longstanding hard requirement across every style
    // added so far.)
    playbackStyleProbabilities = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    // Filter Down/Filter Up (Step 29/30): fixed type/resonance, set once
    // here rather than per-sample -- only the cutoff frequency needs to
    // change during playback (see processBlock()). Sample rate/channel
    // count get set properly in prepareToPlay(); the defaults here are
    // harmless placeholders until then.
    filterSweepFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    filterSweepFilter.setResonance (filterSweepResonance);
}

SlicerAudioProcessor::~SlicerAudioProcessor() = default;

void SlicerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    hasCurrentPick = false;
    clockModeInitialized = false;
    clockCurrentPickValid = false;
    resetWindowInitialized = false; // Step 34

    // Filter Sweep (Step 29) -- must be prepared with the real sample rate
    // before setCutoffFrequency() means anything; 2 channels covers this
    // plugin's stereo-only output (isBusesLayoutSupported() requires it).
    filterSweepFilter.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, 2 });
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

    if (! sampleLoaded)
        return;

    auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>{};
    const bool hostTransportPlaying = position.hasValue() && position->getIsPlaying();
    const double hostSampleRate = getSampleRate();

    // Audition (Step 25): takes priority over everything below — a raw,
    // generative-engine-bypassing loop of [trimStart, trimEnd), independent
    // of host transport (it has to work even while the transport's
    // stopped, since setting up a trim happens before worrying about
    // sync). Auto-stops the instant the host transport starts playing, so
    // it and the real engine below never talk over each other.
    if (auditionActive.load())
    {
        if (hostTransportPlaying)
        {
            auditionActive.store (false);
            auditionPlaybackPositionForUI.store (-1); // Step 28 -- the playhead indicator must vanish the instant this auto-stop fires, same as the audio itself
        }
        else
        {
            if (hostSampleRate > 0.0)
                renderAudition (buffer, hostSampleRate);

            return;
        }
    }

    if (slices.empty())
        return;

    if (playHead == nullptr)
        return;

    if (! position.hasValue() || ! hostTransportPlaying)
    {
        hasCurrentPick = false; // transport stopped — fresh chain next time it starts
        clockModeInitialized = false;
        clockCurrentPickValid = false;
        resetWindowInitialized = false; // Step 34 -- re-snap to the current reset window next time transport starts
        currentlyPlayingSliceIndexForUI.store (-1);
        return;
    }

    const double hostBpm = position->getBpm().hasValue() ? *position->getBpm() : 120.0;

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
    const bool sequencedMode = (triggerMode.load() == TriggerMode::sequenced);

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

    // Both modes need the host's beat position now (Step 34): Clock mode
    // for its own ticks/windows, and Slice Length mode for its lightweight
    // periodic-reset boundary tracking below -- previously Slice Length
    // mode paced itself purely from slice content length and never looked
    // at ppq at all.
    const double ppqStart = position->getPpqPosition().hasValue() ? *position->getPpqPosition() : 0.0;
    const double ppqPerSample = (hostBpm / 60.0) / hostSampleRate;

    if (clockMode)
    {
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

        resetWindowInitialized = false; // the other two modes' own init state is meaningless here; re-entering either later starts fresh
        sequencedModeInitialized = false;
    }
    else if (sequencedMode)
    {
        clockModeInitialized = false;
        resetWindowInitialized = false;

        // Sequenced Trigger Mode (Step 37) — same "just entered / transport
        // just started" treatment Clock mode gives itself above: force the
        // very first per-sample check below to treat whichever step we're
        // currently inside as new, so it triggers immediately rather than
        // waiting for the next step boundary.
        if (! sequencedModeInitialized)
        {
            sequencedLastStepIndex = -1;
            sequencedModeInitialized = true;
        }

        // Defensive: every dimension-changing setter already calls
        // resetSequencerGrid() under this same lock, so this should never
        // actually be needed, but self-heal rather than risk an
        // out-of-bounds read below if that invariant is ever violated.
        if ((int) sequencerGrid.size() != getSequencerNumRows() * getSequencerNumSteps())
            resetSequencerGrid();
    }
    else
    {
        clockModeInitialized = false; // so re-entering Clock mode later starts fresh
        sequencedModeInitialized = false;

        // Slice Length periodic reset (Step 34) — same "just entered /
        // transport just started, snap to the current window and force an
        // immediate pick" treatment Clock mode gives itself just above.
        if (! resetWindowInitialized)
        {
            const double resetWindowBeats = (double) getResetBarsValue (resetBarsIndex.load()) * 4.0; // 4/4
            const juce::int64 windowIndex = (juce::int64) std::floor (ppqStart / resetWindowBeats);
            resetWindowEndPpq = (double) (windowIndex + 1) * resetWindowBeats;
            resetWindowInitialized = true;
            hasCurrentPick = false; // force a fresh pick aligned to this window right away
        }
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

        // Needed by both modes now (Step 34): Clock mode's own tick/window
        // checks below, and Slice Length mode's reset-boundary check
        // further down — computed once per sample, not once per block, on
        // purpose (see the reset-boundary comment below for why).
        const double samplePpq = ppqStart + (double) i * ppqPerSample;

        if (clockMode)
        {
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

                    // Filter Sweep's Whole Window scope (Step 30) -- reset
                    // ONLY here, on a genuine new-window event, never on an
                    // ordinary per-tick retrigger below, so the sweep stays
                    // continuous across every tick inside this window.
                    samplesSinceWindowStart = 0.0;
                    currentWindowLengthHostSamples = windowBeats * (60.0 / hostBpm) * hostSampleRate;
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

                    // Beat-quantized slice length (either pitch mode's
                    // toggle, Step 24/26) never applies in Clock mode -- its
                    // own tick system already enforces beat-alignment.
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
        else if (sequencedMode)
        {
            // Sequenced Trigger Mode (Step 37): checked every SAMPLE, not
            // once per block -- same per-sample discipline (and same
            // reason) as Clock mode's own tick/window checks and the
            // mandatory Reset feature's boundary check, avoiding the exact
            // bug Step 6 introduced and fixed (a boundary computed once per
            // block from the block's start position silently misses
            // boundaries landing mid-block).
            const double stepBeats = getNoteValueBeats (stepResolutionIndex.load());
            const int totalSteps = getSequencerNumSteps();

            if (stepBeats > 0.0 && totalSteps > 0)
            {
                const juce::int64 absoluteStepIndex = (juce::int64) std::floor (samplePpq / stepBeats);
                const int currentStepIndex = (int) (((absoluteStepIndex % totalSteps) + totalSteps) % totalSteps);

                if (currentStepIndex != sequencedLastStepIndex)
                {
                    sequencedLastStepIndex = currentStepIndex;
                    currentlyPlayingStepIndexForUI.store (currentStepIndex);

                    const int numRows = getSequencerNumRows();
                    int activeRow = -1;

                    for (int row = 0; row < numRows; ++row)
                    {
                        if (sequencerGrid[(size_t) (row * totalSteps + currentStepIndex)])
                        {
                            activeRow = row;
                            break;
                        }
                    }

                    // Structural monophony (Step 37) guarantees at most one
                    // active row per column -- if none is active here,
                    // there's nothing new to trigger; whatever's currently
                    // playing (or silence) just continues per its own
                    // existing completion logic below, same as Clock mode
                    // already does between ticks.
                    if (activeRow >= 0 && activeRow < (int) slices.size())
                    {
                        // Reuse the exact same single-voice render path
                        // every other mode already uses -- force a fresh
                        // start regardless of what's currently playing,
                        // same mechanic already proven in Clock mode's
                        // tick-retriggering and the mandatory Reset
                        // feature. Every Sequenced note plays as
                        // PlaybackStyle::forward unconditionally --
                        // playback-style-per-step is explicitly deferred
                        // past v1, and the whole point of this mode is
                        // that nothing here is randomized.
                        const auto& slice = slices[(size_t) activeRow];
                        currentPlaybackStyle = PlaybackStyle::forward;
                        currentSliceStartSample = slice.startSample;
                        currentSliceLength = slice.endSample - slice.startSample;
                        currentPosition = (double) slice.startSample;
                        currentEndSample = slice.endSample;
                        hasCurrentPick = true;
                        pickJustStarted = true;
                        currentlyPlayingSliceIndexForUI.store (activeRow);

                        // Sequenced mode's own step grid already enforces
                        // beat-alignment for SCHEDULING (every note starts
                        // exactly on a step boundary already) -- same
                        // reasoning Clock mode already uses to exclude
                        // Beat-Quantize (Step 24/26), which is about
                        // DURATION, not start time, and would be redundant
                        // here rather than serve any purpose.
                        currentPickBeatQuantized = false;

                        samplesSincePickStart = 0.0;
                        const double naturalLengthHostSamples =
                            (playbackRate > 0.0) ? ((double) currentSliceLength / playbackRate) : 0.0;
                        currentPickMidpointHostSamples = naturalLengthHostSamples; // unused (Forward-only here), harmless
                        currentPickTapeStopDurationHostSamples = naturalLengthHostSamples; // unused, harmless -- same "set unconditionally" convention used elsewhere

                        // Anticipatory fade (Step 37): cap this note's
                        // length to whichever comes first -- its own
                        // natural length, or the NEXT scheduled active step
                        // anywhere in the grid (structural monophony means
                        // that step, whenever it comes, WILL cut this note
                        // off) -- same established anticipatory-fade
                        // pattern already used for Clock's ticks, Tape
                        // Stop, Filter Sweep, and the mandatory Reset
                        // feature. A bounded forward scan (at most
                        // totalSteps * numRows checks), run once per NOTE
                        // START, not per sample.
                        int stepsUntilNextActive = totalSteps;

                        for (int offset = 1; offset <= totalSteps; ++offset)
                        {
                            const int checkColumn = (currentStepIndex + offset) % totalSteps;
                            bool columnHasActive = false;

                            for (int row2 = 0; row2 < numRows; ++row2)
                            {
                                if (sequencerGrid[(size_t) (row2 * totalSteps + checkColumn)])
                                {
                                    columnHasActive = true;
                                    break;
                                }
                            }

                            if (columnHasActive)
                            {
                                stepsUntilNextActive = offset;
                                break;
                            }
                        }

                        const double samplesUntilNextActiveStep =
                            (double) stepsUntilNextActive * stepBeats * (60.0 / hostBpm) * hostSampleRate;
                        currentPickLengthInHostSamples = juce::jmin (naturalLengthHostSamples, samplesUntilNextActiveStep);
                    }
                }
            }
        }
        else
        {
            // Slice Length mode: pick a fresh slice whenever nothing's
            // playing or the current one has run its course. Tape Stop's
            // "run its course" is time-based (its read position
            // deliberately never reaches currentEndSample -- see
            // currentPickTapeStopDurationHostSamples), unlike Forward/
            // Ping-Pong which are always position-based.

            // Periodic reset (Step 34): checked every SAMPLE, not once per
            // block from the block's start position -- the exact bug
            // Step 6 introduced and fixed was computing a cycle/window
            // boundary once per block, which silently missed boundaries
            // landing mid-block. This reuses Clock mode's own per-sample
            // newWindow check directly (same shape of problem, same fix)
            // rather than re-deriving it. Crossing a boundary cuts off
            // whatever's currently playing right here -- hasCurrentPick
            // false makes the while-loop below pick fresh, starting
            // exactly on this sample -- and advances to the NEXT boundary.
            if (samplePpq >= resetWindowEndPpq)
            {
                const double resetWindowBeatsNow = (double) getResetBarsValue (resetBarsIndex.load()) * 4.0; // 4/4, live-read so a mid-stream dropdown change takes effect at the next boundary, same as Clock reference already does
                const juce::int64 windowIndex = (juce::int64) std::floor (samplePpq / resetWindowBeatsNow);
                resetWindowEndPpq = (double) (windowIndex + 1) * resetWindowBeatsNow;

                hasCurrentPick = false;
            }

            // How much host-sample time remains until the next reset
            // boundary -- capped into every fresh pick's currentPickLength-
            // InHostSamples (and, for Tape Stop, currentPickTapeStopDuration-
            // HostSamples) below: the same established anticipatory-fade
            // pattern already used for Clock's ticks, Tape Stop, and Filter
            // Sweep (all of which already cap their own duration/length to
            // "whichever comes first"), so a pick that's about to get cut
            // off by a reset always fades out cleanly instead of clicking.
            const double samplesUntilReset = juce::jmax (0.0, (resetWindowEndPpq - samplePpq) / ppqPerSample);

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

                // Beat-quantized slice length (Step 24 for Time-Stretch,
                // Step 26 for Repitch) — Slice Length mode only,
                // Forward/Ping-Pong only (Tape Stop and Stretch already
                // deliberately override natural duration, so quantizing
                // would fight rather than serve them). Each pitch mode has
                // its own separate toggle/default, but shares the exact
                // same target-duration calculation (computeBeatQuantizeTarget)
                // — only what currentPickQuantizedStretchRatio gets applied
                // TO differs, and that difference needs no branching here:
                // the render step below already substitutes it for
                // repitchRatio in whichever path (granular hop schedule or
                // direct-read position advance) is actually active for the
                // current pitch mode. Computed once here, at pick-start,
                // and consulted for the rest of this pick's life.
                currentPickBeatQuantized = false;

                const bool beatQuantizeWanted = timeStretchMode ? beatQuantizeSliceLengthEnabled.load()
                                                                 : beatQuantizeSliceLengthEnabledRepitch.load();

                if (beatQuantizeWanted
                    && (currentPlaybackStyle == PlaybackStyle::forward || pingPong)
                    && currentSliceLength > 0)
                {
                    const double originalBpm = getCalculatedOriginalBpm();
                    const auto quantizeResult = computeBeatQuantizeTarget (currentSliceLength, pingPong,
                                                                            sampleSampleRate, originalBpm, hostBpm);

                    if (quantizeResult.quantized)
                    {
                        currentPickBeatQuantized = true;
                        currentPickQuantizedStretchRatio = quantizeResult.stretchRatio;

                        const double quantizedLengthHostSamples = quantizeResult.targetHostSeconds * hostSampleRate;
                        currentPickLengthInHostSamples = quantizedLengthHostSamples;

                        if (pingPong)
                            currentPickMidpointHostSamples = quantizedLengthHostSamples * 0.5;
                    }
                }

                // Periodic reset (Step 34): applied LAST, after beat-quantize
                // may have already substituted a different target duration --
                // reset always wins if it would cut the pick shorter than
                // whatever duration was otherwise chosen, same "whichever
                // comes first" precedence Clock mode's own tick/window caps
                // already use. currentPickTapeStopDurationHostSamples is
                // capped unconditionally too (harmless no-op for every style
                // but Tape Stop, which is exactly how that variable already
                // behaves elsewhere in this function).
                currentPickLengthInHostSamples = juce::jmin (currentPickLengthInHostSamples, samplesUntilReset);
                currentPickTapeStopDurationHostSamples = juce::jmin (currentPickTapeStopDurationHostSamples, samplesUntilReset);

                if (++pickAttempts > 1000)
                    return; // safety bail — render the rest of this block as silence
            }
        }

        if (pickJustStarted)
        {
            granularStretcher.reset (currentPosition); // matches whichever mode's active; harmless if unused this pick
            filterSweepFilter.reset(); // Step 29 -- no bleed from a previous pick; harmless if unused this pick
        }

        const bool pingPongActive = (currentPlaybackStyle == PlaybackStyle::pingPong);
        const bool tapeStopActive = (currentPlaybackStyle == PlaybackStyle::tapeStop);
        const bool stretchActive = (currentPlaybackStyle == PlaybackStyle::stretch);
        const bool filterSweepActive = (currentPlaybackStyle == PlaybackStyle::filterSweepDown
                                         || currentPlaybackStyle == PlaybackStyle::filterSweepUp);

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

            // Filter Down/Filter Up (Step 29/30): cutoff computed once per
            // sample here (shared across every output channel below, not
            // recomputed per channel). Log-scale interpolation -- not
            // linear Hz -- between start/end frequency, since frequency
            // perception is logarithmic; this is what keeps the sweep
            // sounding musically even rather than front-loaded. Filter Down
            // sweeps filterSweepStartHz -> filterSweepEndHz (open to
            // closed, the classic breakbeat/DnB "filter close"); Filter Up
            // is the mirror image, filterSweepEndHz -> filterSweepStartHz.
            //
            // The progress fraction itself depends on Filter Sweep scope
            // (Clock mode only -- Slice Length mode has no concept of a
            // "window", so it always behaves like Per Tick regardless of
            // the scope setting, same as before this scope choice existed):
            //   Per Tick (default) -- samplesSincePickStart /
            //     currentPickLengthInHostSamples, resetting at every
            //     retrigger -- today's Step 29 behaviour, unchanged.
            //   Whole Window -- samplesSinceWindowStart / currentWindow-
            //     LengthHostSamples instead, continuous across every tick
            //     in the window (ticks keep retriggering normally --
            //     unlike Tape Stop's Whole Window, nothing here overrides
            //     that), only resetting when a new window begins.
            float filterSweepCutoffHz = filterSweepStartHz;

            if (filterSweepActive)
            {
                const bool useWholeWindow = clockMode && (filterSweepScope.load() == FilterSweepScope::wholeWindow);

                const double progress = useWholeWindow
                    ? ((currentWindowLengthHostSamples > 0.0)
                           ? juce::jlimit (0.0, 1.0, samplesSinceWindowStart / currentWindowLengthHostSamples)
                           : 1.0)
                    : ((currentPickLengthInHostSamples > 0.0)
                           ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                           : 1.0);

                const bool isUp = (currentPlaybackStyle == PlaybackStyle::filterSweepUp);
                const double sweepStartHz = isUp ? (double) filterSweepEndHz : (double) filterSweepStartHz;
                const double sweepEndHz = isUp ? (double) filterSweepStartHz : (double) filterSweepEndHz;

                filterSweepCutoffHz = (float) (sweepStartHz * std::pow (sweepEndHz / sweepStartHz, progress));
                filterSweepFilter.setCutoffFrequency (filterSweepCutoffHz);
            }

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
                    float outputSample = channelSums[srcCh] * (float) gain;

                    if (filterSweepActive)
                        outputSample = filterSweepFilter.processSample (outCh, outputSample);

                    buffer.addSample (outCh, i, outputSample);
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
                    float sample = (s0 + frac * (s1 - s0)) * (float) gain;

                    if (filterSweepActive)
                        sample = filterSweepFilter.processSample (outCh, sample);

                    buffer.addSample (outCh, i, sample);
                }
            }

            // Tape Stop layers its rate multiplier onto the SAME shared
            // advance both pitch modes' scheduling position relies on --
            // this is what makes the read position (and therefore
            // schedulingEndSample/foldedReadPosition above) actually slow
            // down and, as the spec calls out, deliberately fall short of
            // the slice's real end sample in real time. Beat-quantized
            // slice length substitutes this pick's own quantized stretch
            // ratio for repitchRatio here too, in exactly the same place
            // playbackRate itself is built from repitchRatio above -- in
            // Time-Stretch mode (Step 24) that keeps currentPosition
            // landing precisely on the quantized target duration, matching
            // the granular hop schedule above sample for sample; in Repitch
            // mode (Step 26) this line IS the varispeed playback rate for
            // the direct-read path below, so the same substitution is what
            // "adjusts the normal repitch-mode rate calculation" -- no
            // further pitch-mode-specific code needed anywhere else.
            const double effectivePlaybackRate = tapeStopActive ? (playbackRate * tapeStopRateMultiplier)
                                                : currentPickBeatQuantized ? ((sampleSampleRate / hostSampleRate) * currentPickQuantizedStretchRatio)
                                                                           : playbackRate;
            currentPosition += effectivePlaybackRate;
            samplesSincePickStart += 1.0;
        }

        // Filter Sweep's Whole Window scope (Step 30) needs true window-
        // elapsed real time, not just time spent actively rendering a pick
        // -- unlike samplesSincePickStart above, this increments every
        // sample the window is open, including any silence between a
        // slice naturally finishing early and the next forced tick, so a
        // Whole Window sweep stays locked to the window's actual wall-
        // clock length rather than lagging behind it. Slice Length mode
        // has no concept of a window, so this is simply never consulted
        // there (see the useWholeWindow guard above, which already
        // requires clockMode).
        if (clockMode)
            samplesSinceWindowStart += 1.0;
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
        auditionActive.store (false); // Step 25 -- a stale audition loop from the old buffer/trim makes no sense against the new one
        auditionPlaybackPositionForUI.store (-1); // Step 28 -- and neither does its stale playhead indicator
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

void SlicerAudioProcessor::renderAudition (juce::AudioBuffer<float>& buffer, double hostSampleRate)
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    const int sourceLength = sampleBuffer.getNumSamples();
    const int sourceChannels = sampleBuffer.getNumChannels();
    const int outChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (trimEnd - trimStart <= 0 || sourceLength == 0 || sourceChannels == 0)
        return;

    // Native pitch/speed: sample-rate matching only (correcting for the
    // loaded file's own sample rate vs. the host's) — never repitchRatio,
    // which is what keeps this "exactly the source content" regardless of
    // loopLengthBars/tempo. No fades either — the loop seam is meant to be
    // audibly exposed, not hidden.
    const double auditionRate = sampleSampleRate / hostSampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
        if (auditionPosition < (double) trimStart || auditionPosition >= (double) (trimEnd - 1))
            auditionPosition = (double) trimStart;

        const int idx0 = juce::jlimit (0, sourceLength - 1, (int) auditionPosition);
        const int idx1 = juce::jmin (idx0 + 1, sourceLength - 1);
        const float frac = (float) (auditionPosition - (double) idx0);

        for (int outCh = 0; outCh < outChannels; ++outCh)
        {
            const int srcCh = juce::jmin (outCh, sourceChannels - 1);
            const float s0 = sampleBuffer.getSample (srcCh, idx0);
            const float s1 = sampleBuffer.getSample (srcCh, idx1);
            buffer.addSample (outCh, i, s0 + frac * (s1 - s0));
        }

        auditionPosition += auditionRate;
    }

    // Audition playhead (Step 28) — once per block is plenty for a 30fps
    // UI poll (WaveformDisplay's timer), so this doesn't need to be a
    // per-sample store inside the loop above.
    auditionPlaybackPositionForUI.store ((int) auditionPosition);
}

void SlicerAudioProcessor::rebuildSlicesFromDetectionAndManualPoints (float sensitivity, float holdoffMs)
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    auto autoSlices = transientDetector.detectSlices (sensitivity, holdoffMs, trimStart, trimEnd);

    const juce::ScopedLock sl (sampleLock);

    slices = mergeOnsetsIntoSlices (autoSlices, trimStart, trimEnd);
    sliceProbabilities.assign (slices.size(), 1.0f); // default: even odds across all slices
    resetSequencerGrid(); // Step 37 -- row count just changed
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
        {
            // Quantize detected transients to grid (Step 35) -- applied
            // AFTER exclusion matching, against the ORIGINAL (unquantized)
            // position: an exclusion click targets the raw detected peak
            // the user actually saw on the waveform, and matching against
            // that first is what keeps an exclusion correct regardless of
            // whether quantization is on. Manual points below are never
            // touched by this -- they're pushed as-is, further down.
            const int onsetSample = quantizeTransientsEnabled.load()
                ? quantizeOnsetToGrid (s.startSample, trimStart, trimEnd)
                : s.startSample;
            onsets.push_back (onsetSample);
        }
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

int SlicerAudioProcessor::quantizeOnsetToGrid (int onsetSample, int trimStart, int trimEnd) const
{
    const double gridBeats = getNoteValueBeats (quantizeGridIndex.load());

    if (gridBeats <= 0.0)
        return onsetSample;

    // Same source-tempo derivation used everywhere else in this class
    // (computeBeatQuantizeTarget's naturalBeats calculation for the
    // analogous per-pick feature works the same way): originalBpm/60
    // converts a duration in seconds directly to beats, so there's no
    // need to separately compute a "seconds per beat" intermediate.
    const double originalBpm = getCalculatedOriginalBpm();

    if (originalBpm <= 0.0 || sampleSampleRate <= 0.0)
        return onsetSample;

    const double onsetSeconds = (double) (onsetSample - trimStart) / sampleSampleRate;
    const double onsetBeats = onsetSeconds * (originalBpm / 60.0);
    const double nearestGridStep = std::round (onsetBeats / gridBeats);
    const double quantizedBeats = nearestGridStep * gridBeats;
    const double quantizedSeconds = quantizedBeats * (60.0 / originalBpm);
    const double quantizedSampleDouble = (double) trimStart + quantizedSeconds * sampleSampleRate;

    // Clamped so an onset near either edge of the trim can never quantize
    // to a position outside it -- trimEnd - 1 mirrors the same upper bound
    // addManualSlicePoint()/setTrimStartSample() etc. already use for
    // exactly this reason.
    return juce::jlimit (trimStart, juce::jmax (trimStart, trimEnd - 1), (int) std::llround (quantizedSampleDouble));
}

void SlicerAudioProcessor::resetSequencerGrid()
{
    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();
    sequencerGrid.assign ((size_t) juce::jmax (0, rows * columns), false);
}

bool SlicerAudioProcessor::getSequencerCell (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return false;

    const size_t idx = (size_t) (row * columns + column);
    return idx < sequencerGrid.size() && sequencerGrid[idx];
}

void SlicerAudioProcessor::setSequencerCell (int row, int column, bool active)
{
    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return;

    if ((int) sequencerGrid.size() != rows * columns)
        resetSequencerGrid(); // defensive -- dimensions drifted out from under us somehow

    if (active)
    {
        // Structural monophony (Step 37, v1): clear any other active cell
        // in this SAME COLUMN across every row first, so "only one voice"
        // is true at the INPUT level the instant a pattern is drawn, not
        // just something the playback engine happens to enforce
        // afterward -- and it's what avoids needing a tie-break rule
        // entirely at playback time.
        for (int r = 0; r < rows; ++r)
            sequencerGrid[(size_t) (r * columns + column)] = false;
    }

    sequencerGrid[(size_t) (row * columns + column)] = active;
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
