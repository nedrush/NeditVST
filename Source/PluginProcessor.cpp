#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <iostream> // TEMPORARY DEBUG (Performance mode freeze investigation) -- see FreezeWatchdog

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
        "Forward", "Ping-Pong", "Tape Stop", "Stretch", "Filter Down", "Filter Up", "Bitcrush", "Scratch", "Flanger"
    } };

    // Bitcrush character constants (Step 48/49) -- Sweep In/Out's fixed
    // far end. 48 samples is roughly 1/48th of the original rate (well
    // past the original spec's 1/8-1/16 default range, deliberately more
    // extreme since a SWEEP TARGET is meant to read as "maximally
    // crushed," not "typical"); 1 bit is the most crushed a bit-depth
    // quantizer can go. The Static-mode DEFAULTS these used to hold are no
    // longer fixed constants -- see bitcrushRateReductionGlobalValue/
    // bitcrushBitDepthGlobalValue in PluginProcessor.h (Slice Length/Clock
    // mode parameter panel).
    constexpr float bitcrushRateReductionExtreme = 48.0f;
    constexpr float bitcrushBitDepthExtreme = 1.0f;

    // Flanger character constants -- same "fixed Sweep In/Out extreme, no
    // longer a fixed Static default" shape as Bitcrush's own constants
    // just above (see flangerDelayTimeGlobalValue/flangerMixGlobalValue/
    // flangerFeedbackGlobalValue in PluginProcessor.h). Delay Time's
    // extreme sits at the TOP of its range (10ms, the deepest/most
    // pronounced comb notch) exactly like Sample Rate Reduction's extreme
    // does -- "Static, maxed out" and "fully swept" read as the same
    // amount of character. Mix's extreme is fully wet (1.0) for the same
    // reason -- it's the far end of the 0-100% range the parameter is
    // already documented against. Feedback's extreme (0.88) is
    // deliberately short of the mathematically-stable 1.0 ceiling --
    // comfortably clear of self-oscillation/runaway buildup while still
    // landing on a genuinely resonant, pronounced comb character at
    // "Static, maxed out"/Sweep's far end, same convention as the other
    // two.
    constexpr float flangerDelayTimeMinMs = 0.5f;
    constexpr float flangerDelayTimeExtremeMs = 10.0f;
    constexpr float flangerMixExtreme = 1.0f;
    constexpr float flangerFeedbackExtreme = 0.88f;

    // Sweep mode names (Step 49), shared by every swept parameter's own
    // Mode index -- Bitcrush's Sample Rate Reduction Mode/Bit Depth Mode,
    // and Flanger's Delay Time Mode/Mix Mode -- see SlicerAudioProcessor::
    // isSequencerCellParameterSwept().
    const std::array<const char*, 3> sweepModeNames { {
        "Static", "Sweep In", "Sweep Out"
    } };

    // Volume ramp mode names -- Volume's own Mode option list, parallel to
    // sweepModeNames above but with directional language ("Ramp Up"/"Ramp
    // Down") rather than "Sweep In"/"Sweep Out", since volume has an
    // intuitive up/down sense the other swept parameters' effects don't.
    // Both ramp toward/away from silence (0.0), a fixed extreme like every
    // other swept parameter's Sweep In/Out -- see processBlock()'s
    // sweptVolumeValue.
    const std::array<const char*, 3> volumeRampModeNames { {
        "Static", "Ramp Up", "Ramp Down"
    } };

    // Tape Stop scope names (Step 21).
    const std::array<const char*, SlicerAudioProcessor::numTapeStopScopeOptions> tapeStopScopeNames { {
        "Whole window", "Per tick"
    } };

    // Filter Sweep scope names (Step 30).
    const std::array<const char*, SlicerAudioProcessor::numFilterSweepScopeOptions> filterSweepScopeNames { {
        "Whole window", "Per tick"
    } };

    // Filter Sweep filter type names (Step 46).
    const std::array<const char*, SlicerAudioProcessor::numFilterSweepFilterTypeOptions> filterSweepFilterTypeNames { {
        "Low-pass", "High-pass", "Band-pass"
    } };

    // Curve shape names (Step 46) -- shared by Tape Stop decel and
    // Ping-Pong turnaround fade.
    const std::array<const char*, SlicerAudioProcessor::numCurveShapeOptions> curveShapeNames { {
        "Linear", "Exponential"
    } };

    // Sequencer step parameter names (Step 45/46/47), indexed the same
    // way the per-cell override map's lookups (and the right-click menu)
    // use. Subdivide (index 5, Step 47) is general -- see
    // SlicerAudioProcessor::getApplicableSequencerCellParameters(), which
    // appends it unconditionally rather than listing it per-style here.
    const std::array<const char*, SlicerAudioProcessor::numSequencerCellParameters> sequencerCellParameterNames { {
        "Resonance", "Filter Type", "Curve Shape", "Grain Size", "Grain Speed", "Subdivide",
        "Sample Rate Reduction", "Sample Rate Reduction Mode", "Bit Depth", "Bit Depth Mode", "Rate",
        "Forward Curve", "Backward Curve", "Delay Time", "Delay Time Mode", "Mix", "Mix Mode",
        "Feedback", "Feedback Mode", "Volume", "Volume Mode",
        "Delay Send Amount", "Delay Bus Time", "Delay Bus Feedback",
        "Reverb Send Amount", "Reverb Bus Size", "Reverb Bus Decay"
    } };

    // Step 46: shared 0..1 progress-curve remap for Curve Shape -- Linear
    // is the identity (today's behaviour, unchanged); Exponential eases
    // in (t*t -- slow start, fast finish), which reads as a more natural
    // "surge before stopping" for Tape Stop's decel and a snappier
    // turnaround for Ping-Pong, without needing two separate formulas per
    // style. First-pass curve, not meant to be the only shape ever
    // offered -- see setCurveShape()'s doc comment in PluginProcessor.h.
    double applyCurveShape (double t, int curveShapeIndex)
    {
        if (curveShapeIndex == 1)
            return t * t;

        return t;
    }

    // Slice Length periodic reset (Step 34) -- names and their underlying
    // bar counts, held as a parallel pair rather than a NoteValueOption-
    // style struct since bar counts (not beats) are the natural unit
    // here, and every other place in this codebase already converts bars
    // to beats via "* 4" (4/4) rather than storing beats directly.
    const std::array<const char*, SlicerAudioProcessor::numResetBarsOptions> resetBarsNames { {
        "1 bar", "2 bars", "4 bars", "8 bars"
    } };
    const std::array<int, SlicerAudioProcessor::numResetBarsOptions> resetBarsValues { { 1, 2, 4, 8 } };

    // Sequenced mode's Pattern length (Step 38) -- same parallel-array
    // pattern as the reset-bars pair above, deliberately capped at 4 bars
    // (not 8) per spec.
    const std::array<const char*, SlicerAudioProcessor::numPatternLengthBarsOptions> patternLengthBarsNames { {
        "1 bar", "2 bars", "4 bars"
    } };
    const std::array<int, SlicerAudioProcessor::numPatternLengthBarsOptions> patternLengthBarsValues { { 1, 2, 4 } };
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

juce::String SlicerAudioProcessor::getFilterSweepFilterTypeName (int index)
{
    if (index < 0 || index >= numFilterSweepFilterTypeOptions)
        return {};

    return filterSweepFilterTypeNames[(size_t) index];
}

juce::String SlicerAudioProcessor::getCurveShapeName (int index)
{
    if (index < 0 || index >= numCurveShapeOptions)
        return {};

    return curveShapeNames[(size_t) index];
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

juce::String SlicerAudioProcessor::getPatternLengthBarsName (int index)
{
    if (index < 0 || index >= numPatternLengthBarsOptions)
        return {};

    return patternLengthBarsNames[(size_t) index];
}

int SlicerAudioProcessor::getPatternLengthBarsValue (int index)
{
    if (index < 0 || index >= numPatternLengthBarsOptions)
        return 1; // matches the default index (0 -> 1 bar)

    return patternLengthBarsValues[(size_t) index];
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

double SlicerAudioProcessor::computeScratchCycleLengthHostSamples (int rateIndex, int sliceLength,
                                                                     double hostBpm, double hostSampleRate, double playbackRate)
{
    if (sliceLength <= 0 || hostBpm <= 0.0 || hostSampleRate <= 0.0 || playbackRate <= 0.0)
        return 0.0;

    const double rateBeats = getNoteValueBeats (rateIndex);
    const double desiredCycleHostSamples = rateBeats * (60.0 / hostBpm) * hostSampleRate;
    const double desiredCycleSourceSamples = desiredCycleHostSamples * playbackRate;

    // One LEG of the cycle (half of it, the "there" or "back" half) is
    // clamped to the slice's own content length -- see this function's
    // own doc comment in PluginProcessor.h for why.
    const double clampedLegSourceSamples = juce::jmin (desiredCycleSourceSamples * 0.5, (double) sliceLength);
    return 2.0 * clampedLegSourceSamples / playbackRate;
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
    // Tape Stop/Stretch/Filter Down/Filter Up/Bitcrush/Scratch is ever
    // drawn unless the user explicitly turns its weight up. (Step 22's
    // spec described this as "all other styles at weight 1, Stretch at
    // weight 0," which would actually break that guarantee for existing
    // users -- kept Forward-only here instead, since "must sound identical
    // to current behavior" is the longstanding hard requirement across
    // every style added so far.)
    playbackStyleProbabilities = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    // Filter Down/Filter Up (Step 29/30): fixed type, set once here rather
    // than per-sample -- only the cutoff frequency needs to change during
    // playback (see processBlock()). Resonance is no longer fixed (Step
    // 45) -- see currentPickFilterSweepResonance, captured once per pick
    // and applied in the shared pickJustStarted block instead of here.
    // Sample rate/channel count get set properly in prepareToPlay(); the
    // defaults here are harmless placeholders until then.
    filterSweepFilter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    filterSweepFilter.setResonance (defaultFilterSweepResonance);

    // Performance mode's working state (Pass 1) -- seed its parameter
    // values from the same global defaults Slice Length/Clock start with,
    // rather than zeros (a broken default for parameters like Bit Depth).
    // From here on it's independent storage -- editing it never touches,
    // and is never touched by, the global values this seeded from.
    for (int i = 0; i < numSequencerCellParameters; ++i)
        performanceWorkingState.parameterValues[(size_t) i] = getSequencerCellParameterGlobalValue (i);

#if JUCE_DEBUG
    // TEMPORARY DEBUG (Performance mode click-to-focus freeze investigation)
    // -- see freezeWatchdog's own doc comment. Runs on its own dedicated
    // thread, independent of both the audio thread and the message thread.
    freezeWatchdog.startTimer (1000);
#endif
}

SlicerAudioProcessor::~SlicerAudioProcessor()
{
#if JUCE_DEBUG
    freezeWatchdog.stopTimer();
#endif
}

#if JUCE_DEBUG
void SlicerAudioProcessor::FreezeWatchdog::hiResTimerCallback()
{
    const auto entries = owner.debugAudioProcessBlockEntries.load (std::memory_order_relaxed);
    const auto acquired = owner.debugAudioLockAcquiredCount.load (std::memory_order_relaxed);
    const bool focusInProgress = owner.debugFocusChangeInProgress.load (std::memory_order_relaxed);

    std::cerr << "[watchdog] audioBlockEntries=" << entries << " audioLockAcquired=" << acquired;

    if (focusInProgress)
    {
        const auto startedMs = owner.debugFocusChangeStartMs.load (std::memory_order_relaxed);
        const auto waitedMs = (juce::int64) juce::Time::getMillisecondCounter() - startedMs;
        std::cerr << "  *** UI FOCUS CHANGE (note " << owner.debugFocusChangeNoteNumber.load (std::memory_order_relaxed)
                   << ") STILL IN PROGRESS -- waiting " << waitedMs << "ms ***";
    }

    std::cerr << std::endl;
}
#endif

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

    // Flanger delay line -- same "must be sized against the real sample
    // rate, not a construction-time guess" reasoning as filterSweepFilter
    // just above. Sized to comfortably hold flangerDelayTimeExtremeMs (the
    // Sweep In/Out target, and the slider's own max -- see
    // getSequencerCellParameterMax()) plus a little headroom, so the
    // longest delay this style can ever request always fits without
    // wrapping into not-yet-written buffer content. 2 channels matches
    // filterSweepFilter's own stereo-only assumption above.
    const int flangerDelayBufferLength = juce::jmax (4, juce::roundToInt ((flangerDelayTimeExtremeMs / 1000.0) * sampleRate) + 4);
    flangerDelayBuffer.setSize (GranularStretcher::maxChannels, flangerDelayBufferLength);
    flangerDelayBuffer.clear();
    flangerDelayWriteIndex = 0;

    // Delay/Reverb send buses (Pass 1) -- same "needs the real sample
    // rate, not a construction-time guess" reasoning as filterSweepFilter/
    // flangerDelayBuffer above. Max delay comfortably covers the Delay
    // Time parameter's full 2000ms range with headroom.
    delayBusBuffer.setSize (GranularStretcher::maxChannels, samplesPerBlock);
    delayBusBuffer.clear();
    reverbBusBuffer.setSize (GranularStretcher::maxChannels, samplesPerBlock);
    reverbBusBuffer.clear();

    delayBusLine.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) GranularStretcher::maxChannels });
    delayBusLine.setMaximumDelayInSamples (juce::roundToInt (2.5 * sampleRate));
    delayBusLine.reset();

    reverbBusDSP.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) GranularStretcher::maxChannels });
    reverbBusDSP.reset();
}

void SlicerAudioProcessor::releaseResources() {}

bool SlicerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SlicerAudioProcessor::renderPickBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

#if JUCE_DEBUG
    // TEMPORARY DEBUG (Performance mode freeze investigation) -- lock-free
    // atomic store only, see freezeWatchdog's own doc comment.
    debugAudioProcessBlockEntries.fetch_add (1, std::memory_order_relaxed);
#endif

    const juce::ScopedLock sl (sampleLock);

#if JUCE_DEBUG
    debugAudioLockAcquiredCount.fetch_add (1, std::memory_order_relaxed);
#endif

    // Computed ahead of the MIDI dispatch just below (rather than after, as
    // before Quantize Recall) purely so handlePerformanceStateNoteOn() can
    // know whether the host transport is playing AT NOTE-ON TIME, to decide
    // immediate-vs-deferred -- everything else that reads these three still
    // does so no earlier than it did before, so this reordering is a no-op
    // for every other mode.
    auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>{};
    const bool hostTransportPlaying = position.hasValue() && position->getIsPlaying();

    // Dispatch note-ons before discarding the buffer below -- see
    // handleIncomingMidi()/dispatchNoteOn() for the routing layer. Done
    // before the trigger-mode init section further down (which runs once
    // per block, later in this same call) so a MIDI-triggered pattern
    // recall's dimension/state changes are already in place when that
    // section runs, giving an instant switch within the block the note-on
    // arrived in.
    handleIncomingMidi (midiMessages, hostTransportPlaying);
    midiMessages.clear(); // not a MIDI effect -- never produce MIDI out

    if (! sampleLoaded)
        return;

    const double hostSampleRate = getSampleRate();

    // Performance mode (Pass 1) needs to be known before the gates below --
    // same "runs independent of host transport" precedent as Audition just
    // underneath, just reached via a condition on those gates rather than a
    // structural early-return, since Performance mode shares the rest of
    // this function's per-sample render loop with every other mode
    // (Audition doesn't -- it renders through its own separate function and
    // returns immediately, so it never needs to reach the gates at all).
    const bool performanceMode = (triggerMode.load() == TriggerMode::performance);

    // Control mode (same reasoning as performanceMode just above) --
    // note-on-driven slice triggering has to work whether or not the host
    // transport is running, unlike Slice Length/Clock/Sequenced.
    const bool controlMode = (triggerMode.load() == TriggerMode::control);

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

    // Performance mode (Pass 1) and Control mode are both exempt from both
    // gates below, same "independent of host transport" contract Audition
    // already gets above -- recalling a state (or hearing the one currently
    // being edited), or triggering a slice directly from an incoming note,
    // has to work whether or not the host's transport is running, unlike
    // Slice Length/Clock/Sequenced, which are all genuinely meaningless
    // without it (their triggers ARE the transport's beat/bar position).
    if (playHead == nullptr && ! performanceMode && ! controlMode)
        return;

    if ((! position.hasValue() || ! hostTransportPlaying) && ! performanceMode && ! controlMode)
    {
        hasCurrentPick = false; // transport stopped — fresh chain next time it starts
        clockModeInitialized = false;
        clockCurrentPickValid = false;
        resetWindowInitialized = false; // Step 34 -- re-snap to the current reset window next time transport starts
        currentlyPlayingSliceIndexForUI.store (-1);
        return;
    }

    // position may be absent, or reporting "not playing," here -- only
    // possible for a Performance mode block, since the gate above already
    // returned for every other mode in that case. Same 120bpm/0ppq fallback
    // this line already used for a host that simply doesn't report BPM
    // while playing, extended to cover "doesn't report position at all."
    const double hostBpm = (position.hasValue() && position->getBpm().hasValue()) ? *position->getBpm() : 120.0;

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
    // performanceMode was already determined above, ahead of the transport gates it needs to bypass

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
    const double ppqStart = (position.hasValue() && position->getPpqPosition().hasValue()) ? *position->getPpqPosition() : 0.0;
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

        resetWindowInitialized = false; // the other modes' own init state is meaningless here; re-entering any of them later starts fresh
        sequencedModeInitialized = false;
        performanceModeInitialized = false;
        controlModeInitialized = false;
    }
    else if (sequencedMode)
    {
        clockModeInitialized = false;
        resetWindowInitialized = false;
        performanceModeInitialized = false;
        controlModeInitialized = false;

        // Sequenced Trigger Mode (Step 37) — same "just entered / transport
        // just started" treatment Clock mode gives itself above: force the
        // very first per-sample check below to treat whichever step we're
        // currently inside as new, so it triggers immediately rather than
        // waiting for the next step boundary.
        if (! sequencedModeInitialized)
        {
            sequencedLastStepIndex = -1;
            sequencedModeInitialized = true;
            sequencedSubdivisionActive = false; // Step 47 -- no stale retrigger schedule carried in from before this mode was (re-)entered
        }

        // Defensive: every dimension-changing setter already calls
        // resetSequencerGrid() under this same lock, so this should never
        // actually be needed, but self-heal rather than risk an
        // out-of-bounds read below if that invariant is ever violated.
        if ((int) sequencerGrid.size() != getSequencerNumRows() * getSequencerNumSteps())
            resetSequencerGrid();
    }
    else if (performanceMode)
    {
        clockModeInitialized = false;
        resetWindowInitialized = false;
        sequencedModeInitialized = false;
        controlModeInitialized = false;

        // Just entered Performance mode, or transport just started --
        // unlike every other mode's own init above, this does NOT force an
        // immediate fresh pick: Performance mode has nothing to play until
        // a note-on recalls a state (performanceRecallPending, consumed in
        // the per-sample dispatch below), so it starts and stays silent.
        if (! performanceModeInitialized)
        {
            performanceModeInitialized = true;
            hasCurrentPick = false;
        }
    }
    else if (controlMode)
    {
        clockModeInitialized = false;
        resetWindowInitialized = false;
        sequencedModeInitialized = false;
        performanceModeInitialized = false;

        // Just entered Control mode, or transport just started -- same
        // "starts and stays silent until a note-on" contract Performance
        // mode's own init gives itself just above.
        if (! controlModeInitialized)
        {
            controlModeInitialized = true;
            hasCurrentPick = false;
            controlGateReleaseActive = false;
            controlCurrentlySoundingNote = -1;
        }
    }
    else
    {
        clockModeInitialized = false; // so re-entering Clock mode later starts fresh
        sequencedModeInitialized = false;
        performanceModeInitialized = false; // so re-entering Performance mode later starts fresh (silent) again
        controlModeInitialized = false; // so re-entering Control mode later starts fresh (silent) again

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
                    const bool scratch = (clockCurrentPlaybackStyle == PlaybackStyle::scratch);

                    // Stretch grain settings (Step 46) -- Clock mode always
                    // uses the global values; per-step overrides are
                    // Sequenced-mode-only. Captured here, before
                    // currentEndSample below, since Grain Speed feeds
                    // directly into that calculation.
                    currentPickStretchGrainSizeMs = stretchGrainSizeMsValue.load();
                    currentPickStretchSpeedMultiplier = stretchSpeedMultiplierValue.load();

                    currentPlaybackStyle = clockCurrentPlaybackStyle;
                    currentSliceStartSample = slice.startSample;
                    currentSliceLength = slice.endSample - slice.startSample;
                    currentPosition = (double) slice.startSample;

                    // Scratch (v1) -- Clock mode always uses the global
                    // Rate value; per-step overrides are Sequenced-mode-
                    // only, same pattern as Bitcrush above. Captured here,
                    // before currentEndSample below, since it feeds
                    // directly into that calculation, same as Grain Speed
                    // does for Stretch.
                    currentPickScratchCycleLengthHostSamples = scratch
                        ? computeScratchCycleLengthHostSamples (getScratchRateGlobal(), currentSliceLength,
                                                                 hostBpm, hostSampleRate, playbackRate)
                        : 0.0;

                    // Forward/Backward Curve (Scratch v2) -- Clock mode
                    // always uses the global values; per-step overrides
                    // are Sequenced-mode-only, same pattern as Rate just
                    // above.
                    currentPickScratchForwardCurve = easingCurveFromIndex (getScratchForwardCurveGlobal());
                    currentPickScratchBackwardCurve = easingCurveFromIndex (getScratchBackwardCurveGlobal());

                    currentEndSample = pingPong ? (2 * slice.endSample - slice.startSample)
                                     : stretch ? (int) (slice.startSample + (double) currentPickStretchSpeedMultiplier * currentSliceLength)
                                     : scratch ? (int) (slice.startSample + currentPickScratchCycleLengthHostSamples * playbackRate)
                                               : slice.endSample;
                    hasCurrentPick = true;
                    pickJustStarted = true;
                    currentlyPlayingSliceIndexForUI.store (clockCurrentSliceIndex);

                    // Beat-quantized slice length (either pitch mode's
                    // toggle, Step 24/26) never applies in Clock mode -- its
                    // own tick system already enforces beat-alignment.
                    currentPickBeatQuantized = false;
                    currentPickNativeRateActive = false; // Performance mode's Sync-off override never applies outside Performance mode's own picks

                    // Filter Sweep resonance/filter type + Curve Shape
                    // (Step 45/46) -- Clock mode always uses the global
                    // values; per-step overrides are Sequenced-mode-only.
                    currentPickFilterSweepResonance = filterSweepResonanceValue.load();
                    currentPickFilterSweepType = filterSweepFilterTypeValue.load();
                    currentPickCurveShape = curveShapeValue.load();

                    // Bitcrush Sample Rate Reduction/Bit Depth (Step 49) --
                    // Clock mode always uses the global values; per-step
                    // value/mode overrides are Sequenced-mode-only, same
                    // pattern as Filter Sweep just above.
                    currentPickBitcrushRateValue = getBitcrushRateReductionGlobal();
                    currentPickBitcrushRateMode = getBitcrushRateReductionModeGlobal();
                    currentPickBitcrushBitDepthValue = getBitcrushBitDepthGlobal();
                    currentPickBitcrushBitDepthMode = getBitcrushBitDepthModeGlobal();

                    // Flanger Delay Time/Mix/Feedback -- Clock mode always
                    // uses the global values; per-step value/mode overrides
                    // are Sequenced-mode-only, same pattern as Bitcrush
                    // just above.
                    currentPickFlangerDelayValue = getFlangerDelayTimeGlobal();
                    currentPickFlangerDelayMode = getFlangerDelayTimeModeGlobal();
                    currentPickFlangerMixValue = getFlangerMixGlobal();
                    currentPickFlangerMixMode = getFlangerMixModeGlobal();
                    currentPickFlangerFeedbackValue = getFlangerFeedbackGlobal();
                    currentPickFlangerFeedbackMode = getFlangerFeedbackModeGlobal();

                    samplesSincePickStart = 0.0;
                    const double naturalLengthHostSamples =
                        (playbackRate > 0.0) ? ((double) currentSliceLength / playbackRate) : 0.0;

                    // Where a Ping-Pong round trip reverses direction —
                    // always one slice's worth of natural playback time,
                    // regardless of whether the tick below ends up cutting
                    // the pick off before ever reaching it. Unused for
                    // Forward. Scratch (v1) overrides this to half its OWN
                    // cycle length instead -- see currentPickScratchCycle-
                    // LengthHostSamples above -- since its bounce has
                    // nothing to do with the slice's natural length.
                    currentPickMidpointHostSamples = scratch
                        ? (currentPickScratchCycleLengthHostSamples * 0.5)
                        : naturalLengthHostSamples;

                    const double roundTripLengthHostSamples = pingPong ? (2.0 * naturalLengthHostSamples)
                                                             : scratch ? currentPickScratchCycleLengthHostSamples
                                                                       : naturalLengthHostSamples;

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
                        // first: the full currentPickStretchSpeedMultiplier-x
                        // natural length, or the window's own boundary
                        // (mirrors Forward/Ping-Pong's own "whichever comes
                        // first" clamp against a tick, just against the
                        // window instead, since there's no tick to speak of
                        // here).
                        currentPickLengthInHostSamples = juce::jmin ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples,
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
            double stepBeats = getNoteValueBeats (stepResolutionIndex.load());
            int totalSteps = getSequencerNumSteps();

            if (stepBeats > 0.0 && totalSteps > 0)
            {
                juce::int64 absoluteStepIndex = (juce::int64) std::floor (samplePpq / stepBeats);
                int currentStepIndex = (int) (((absoluteStepIndex % totalSteps) + totalSteps) % totalSteps);

                // Pattern Switch Timing (Pass 2): Set Interval/End of
                // Pattern both defer an already-armed recall
                // (pendingPatternSwitchNote) to a boundary, checked every
                // SAMPLE right here alongside the step-boundary check just
                // below it -- same per-sample discipline as everything else
                // in this function, avoiding the Step 6 bug (a boundary
                // computed once per block silently missing ones that land
                // mid-block). Deliberately evaluated against THIS pattern's
                // stepBeats/totalSteps (captured above, before any swap
                // below) -- "reaches the end of its OWN Pattern Length"
                // means the currently playing pattern's own length, not the
                // incoming one's.
                const int pendingSwitchNote = pendingPatternSwitchNote.load();

                if (pendingSwitchNote >= 0)
                {
                    bool boundaryCrossed = false;

                    if (patternSwitchTiming.load() == PatternSwitchTiming::setInterval)
                    {
                        if (! patternSwitchIntervalBoundaryArmed)
                        {
                            // Just (re-)armed -- snap to the next grid point
                            // from wherever ppq is right now, same
                            // "windowIndex + 1" shape Clock mode/Reset use
                            // to find their own next boundary.
                            const double intervalBeats = juce::jmax (
                                getNoteValueBeats (patternSwitchIntervalIndex.load()), 1.0e-6);
                            const juce::int64 intervalIndex = (juce::int64) std::floor (samplePpq / intervalBeats);
                            patternSwitchIntervalBoundaryPpq = (double) (intervalIndex + 1) * intervalBeats;
                            patternSwitchIntervalBoundaryArmed = true;
                        }

                        boundaryCrossed = (samplePpq >= patternSwitchIntervalBoundaryPpq);
                    }
                    else if (patternSwitchTiming.load() == PatternSwitchTiming::endOfPattern)
                    {
                        // The pattern's own existing loop-boundary logic --
                        // the same absoluteStepIndex/totalSteps wrap the
                        // step-trigger check just below already relies on
                        // -- not a separately calculated boundary. A
                        // genuine wrap (step totalSteps-1 -> step 0), not
                        // just "step index happens to be 0" (which is also
                        // true on the very first step ever, before
                        // sequencedLastStepIndex has taken any real value).
                        boundaryCrossed = (currentStepIndex == 0 && sequencedLastStepIndex == totalSteps - 1);
                    }

                    if (boundaryCrossed)
                    {
                        handleSequencerPatternRecallNoteOn (pendingSwitchNote);
                        pendingPatternSwitchNote.store (-1);
                        patternSwitchIntervalBoundaryArmed = false;

                        // handleSequencerPatternRecallNoteOn() just forced
                        // sequencedModeInitialized false expecting the
                        // usual once-per-block re-sync (see its own doc
                        // comment) -- but that block-level check already
                        // ran for THIS block, before this per-sample loop
                        // started, so left alone that flag would instead
                        // fire a redundant second re-sync at the START of
                        // NEXT block, re-triggering the same step again.
                        // Set it back to true here since this sample's own
                        // step alignment is being re-derived immediately
                        // below against the just-recalled pattern instead.
                        sequencedModeInitialized = true;

                        // Re-derive against the just-recalled pattern's own
                        // resolution/length so the step lookup below reads
                        // the NEW grid at the correct width, not the old
                        // pattern's.
                        stepBeats = getNoteValueBeats (stepResolutionIndex.load());
                        totalSteps = getSequencerNumSteps();

                        if (stepBeats > 0.0 && totalSteps > 0)
                        {
                            absoluteStepIndex = (juce::int64) std::floor (samplePpq / stepBeats);
                            currentStepIndex = (int) (((absoluteStepIndex % totalSteps) + totalSteps) % totalSteps);
                        }
                    }
                }

                if (stepBeats > 0.0 && totalSteps > 0 && currentStepIndex != sequencedLastStepIndex)
                {
                    sequencedLastStepIndex = currentStepIndex;
                    currentlyPlayingStepIndexForUI.store (currentStepIndex);

                    const int numRows = getSequencerNumRows();
                    int activeRow = -1;
                    int activeStyle = -1;

                    for (int row = 0; row < numRows; ++row)
                    {
                        const int style = sequencerGrid[(size_t) (row * totalSteps + currentStepIndex)];

                        if (style >= 0)
                        {
                            activeRow = row;
                            activeStyle = style;
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
                        // feature. Each note plays as whichever
                        // PlaybackStyle its own cell stores (Step 41) --
                        // selected directly, not via a weighted draw, since
                        // the whole point of this mode is that everything
                        // is explicitly placed by the user.
                        const auto& slice = slices[(size_t) activeRow];
                        currentPlaybackStyle = indexToPlaybackStyle (activeStyle);
                        const bool tapeStop = (currentPlaybackStyle == PlaybackStyle::tapeStop);
                        const bool stretch = (currentPlaybackStyle == PlaybackStyle::stretch);
                        const bool scratch = (currentPlaybackStyle == PlaybackStyle::scratch);

                        // Stretch grain settings (Step 46) -- this step's
                        // own overrides if it has them, else the global
                        // values, same per-step-override pattern as Filter
                        // Sweep resonance below. Captured here (before
                        // currentEndSample below), since Grain Speed feeds
                        // directly into that calculation, unlike Resonance/
                        // Filter Type/Curve Shape which are render-only and
                        // can be captured later.
                        currentPickStretchGrainSizeMs = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Grain Size", stretchGrainSizeMsValue.load());
                        currentPickStretchSpeedMultiplier = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Grain Speed", stretchSpeedMultiplierValue.load());

                        currentSliceStartSample = slice.startSample;
                        currentSliceLength = slice.endSample - slice.startSample;
                        currentPosition = (double) slice.startSample;

                        // Ping-Pong here (Step 44) deliberately does NOT
                        // double currentEndSample the way Slice Length/
                        // Clock modes' round trip does -- it plays a
                        // half-content window instead (see the halved
                        // fold-length in the shared render code below), so
                        // the full there-and-back cycle only needs to span
                        // the slice's original raw length once, exactly
                        // like every other style here. Stretch (Step-
                        // extension fix) no longer gets its own extended
                        // value here either -- its render-continue gate is
                        // now duration-based (currentPickLengthInHostSamples,
                        // set below), the same reasoning Tape Stop's own
                        // gate switch used, so currentEndSample no longer
                        // needs to (and, since Grain Speed isn't a duration
                        // multiplier anymore, no longer sensibly CAN)
                        // encode Stretch's play length -- just the slice's
                        // own natural end, like every other style here.
                        currentEndSample = slice.endSample;
                        hasCurrentPick = true;
                        pickJustStarted = true;
                        currentlyPlayingSliceIndexForUI.store (activeRow);

                        // Filter Sweep resonance/filter type + Curve Shape
                        // (Step 45/46) -- this step's own override if it
                        // has one, else the global value. Looked up
                        // unconditionally, harmless for styles that don't
                        // use a given parameter (an override is only ever
                        // populated for cells whose style actually offers
                        // it, so the lookup naturally falls through to the
                        // global default there anyway).
                        currentPickFilterSweepResonance = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Resonance", filterSweepResonanceValue.load());
                        currentPickFilterSweepType = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Filter Type", (float) filterSweepFilterTypeValue.load()));
                        currentPickCurveShape = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Curve Shape", (float) curveShapeValue.load()));

                        // Bitcrush Sample Rate Reduction/Bit Depth VALUE +
                        // MODE (Step 49) -- same per-step-override-else-
                        // global pattern as Filter Sweep just above. Mode
                        // overrides are stored under "<name> Mode" (see
                        // SequencerGrid::showParameterMenuForCell()'s swept
                        // branch), separately from the value itself.
                        currentPickBitcrushRateValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Sample Rate Reduction", getBitcrushRateReductionGlobal());
                        currentPickBitcrushRateMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Sample Rate Reduction Mode", (float) getBitcrushRateReductionModeGlobal()));
                        currentPickBitcrushBitDepthValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Bit Depth", getBitcrushBitDepthGlobal());
                        currentPickBitcrushBitDepthMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Bit Depth Mode", (float) getBitcrushBitDepthModeGlobal()));

                        // Flanger Delay Time/Mix/Feedback VALUE + MODE --
                        // same per-step-override-else-global pattern as
                        // Bitcrush just above.
                        currentPickFlangerDelayValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Delay Time", getFlangerDelayTimeGlobal());
                        currentPickFlangerDelayMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Delay Time Mode", (float) getFlangerDelayTimeModeGlobal()));
                        currentPickFlangerMixValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Mix", getFlangerMixGlobal());
                        currentPickFlangerMixMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Mix Mode", (float) getFlangerMixModeGlobal()));
                        currentPickFlangerFeedbackValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Feedback", getFlangerFeedbackGlobal());
                        currentPickFlangerFeedbackMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Feedback Mode", (float) getFlangerFeedbackModeGlobal()));

                        // Volume ramp VALUE + MODE -- style-independent, so
                        // unlike Bitcrush/Flanger above there's no global
                        // dial to fall back to (same as Subdivide); an
                        // absent override falls back straight to 1.0/Static
                        // (full volume, unchanged), matching
                        // getSequencerCellParameterGlobalValue()'s own
                        // fallback for indices 19/20.
                        currentPickVolumeValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Volume", 1.0f);
                        currentPickVolumeMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Volume Mode", 0.0f));

                        // Send-to-bus (Pass 2) -- Send Amount is style-
                        // independent and opt-in, same "no global dial,
                        // captured only here" pattern as Volume just above:
                        // an absent override falls back to 0.0f (no send),
                        // read directly by processSendBuses() for the
                        // duration of this pick.
                        currentPickDelaySendAmount = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Delay Send Amount", 0.0f);
                        currentPickReverbSendAmount = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Reverb Send Amount", 0.0f);

                        // Send-to-bus character overrides (Time/Feedback/
                        // Size/Decay) -- unlike every value above, these
                        // don't feed this pick's own rendering at all; they
                        // instead re-target the SHARED bus's own live ramp
                        // (see delayBusTimeMsRampTargetValue etc. and
                        // processSendBuses()), so the effect persists and
                        // keeps evolving after this pick finishes, exactly
                        // matching this feature's "one shared bus,
                        // reconfigured over time" spec. Only touched when
                        // this step actually HAS an override for that one
                        // parameter -- a step with no override leaves the
                        // target (and therefore the bus) exactly where it
                        // already was, per spec.
                        if (getSequencerCellHasParameterOverride (activeRow, currentStepIndex, "Delay Bus Time"))
                            delayBusTimeMsRampTargetValue.store (juce::jlimit (1.0f, 2000.0f,
                                getSequencerCellParameterOverride (activeRow, currentStepIndex, "Delay Bus Time", 0.0f)));

                        if (getSequencerCellHasParameterOverride (activeRow, currentStepIndex, "Delay Bus Feedback"))
                            delayBusFeedbackRampTargetValue.store (juce::jlimit (0.0f, 0.95f,
                                getSequencerCellParameterOverride (activeRow, currentStepIndex, "Delay Bus Feedback", 0.0f)));

                        if (getSequencerCellHasParameterOverride (activeRow, currentStepIndex, "Reverb Bus Size"))
                            reverbBusSizeRampTargetValue.store (juce::jlimit (0.0f, 1.0f,
                                getSequencerCellParameterOverride (activeRow, currentStepIndex, "Reverb Bus Size", 0.0f)));

                        if (getSequencerCellHasParameterOverride (activeRow, currentStepIndex, "Reverb Bus Decay"))
                            reverbBusDecayRampTargetValue.store (juce::jlimit (0.0f, 1.0f,
                                getSequencerCellParameterOverride (activeRow, currentStepIndex, "Reverb Bus Decay", 0.0f)));

                        // Scratch Rate (v1) -- this step's own override if
                        // it has one, else the global value, same per-
                        // step-override-else-global pattern as Filter
                        // Sweep/Bitcrush above.
                        const int scratchRateIndex = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Rate", (float) getScratchRateGlobal()));
                        currentPickScratchCycleLengthHostSamples = scratch
                            ? computeScratchCycleLengthHostSamples (scratchRateIndex, currentSliceLength,
                                                                     hostBpm, hostSampleRate, playbackRate)
                            : 0.0;

                        // Forward/Backward Curve (Scratch v2) -- this
                        // step's own override if it has one, else the
                        // global value, same per-step-override-else-global
                        // pattern as Rate just above.
                        currentPickScratchForwardCurve = easingCurveFromIndex (juce::roundToInt (
                            getSequencerCellParameterOverride (activeRow, currentStepIndex, "Forward Curve", (float) getScratchForwardCurveGlobal())));
                        currentPickScratchBackwardCurve = easingCurveFromIndex (juce::roundToInt (
                            getSequencerCellParameterOverride (activeRow, currentStepIndex, "Backward Curve", (float) getScratchBackwardCurveGlobal())));

                        // Sequenced mode's own step grid already enforces
                        // beat-alignment for SCHEDULING (every note starts
                        // exactly on a step boundary already) -- same
                        // reasoning Clock mode already uses to exclude
                        // Beat-Quantize (Step 24/26), which is about
                        // DURATION, not start time, and would be redundant
                        // here rather than serve any purpose.
                        currentPickBeatQuantized = false;
                        currentPickNativeRateActive = false; // Performance mode's Sync-off override never applies outside Performance mode's own picks

                        samplesSincePickStart = 0.0;
                        const double naturalLengthHostSamples =
                            (playbackRate > 0.0) ? ((double) currentSliceLength / playbackRate) : 0.0;
                        currentPickMidpointHostSamples = scratch
                            ? (currentPickScratchCycleLengthHostSamples * 0.5)
                            : naturalLengthHostSamples; // where a Ping-Pong round trip reverses; unused otherwise -- same natural-length-based timing as every other trigger mode (Step 44), unchanged by the half-content window below. Scratch (v1) uses half its OWN cycle length instead, same override reasoning as Clock mode's own pick-start.

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
                                if (sequencerGrid[(size_t) (row2 * totalSteps + checkColumn)] >= 0)
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

                        // Style-dependent duration (Step 41/43) -- mirrors
                        // Clock mode's own per-tick calc exactly (this
                        // mode's "next active step" plays the same role as
                        // Clock's tick/window boundary), since Sequenced
                        // mode has no "Whole Window" equivalent -- Tape
                        // Stop and Filter Sweep always behave as Per Tick
                        // here, which needs no extra code: Filter Sweep's
                        // own Whole Window check is already gated to
                        // clockMode elsewhere, and Tape Stop's duration
                        // below is driven entirely by the schedule, same as
                        // Clock's own Per Tick branch. Ping-Pong (Step 44)
                        // needs no special case here at all -- its
                        // half-content window (see the shared render code
                        // below) means its total duration is just a normal
                        // single pick's length, same as Forward/Filter
                        // Sweep share in the universal clamp below.
                        if (tapeStop)
                        {
                            // Step-extension fix: Tape Stop's decel is
                            // capped by THIS step's own declared length --
                            // natural (Step-resolution-quantized) slice
                            // length, or its Step-extension override if
                            // Shift+drag set one -- the identical value
                            // SequencerGrid's piano-roll bar renders (see
                            // getSequencerCellDeclaredLengthSteps()),
                            // converted from steps to host samples via the
                            // same stepBeats/hostBpm/hostSampleRate used
                            // for samplesUntilNextActiveStep above. Still
                            // clamped against samplesUntilNextActiveStep,
                            // same "whichever comes first" precedence the
                            // bar's own computeBarLengthInSteps() already
                            // applies (its next-active-step clamp, run
                            // AFTER settling on desiredSteps) -- a step's
                            // own length is the ceiling, but a genuinely
                            // upcoming step still cuts it off early, same
                            // as what's actually drawn.
                            const int declaredLengthSteps = getSequencerCellDeclaredLengthSteps (activeRow, currentStepIndex);
                            const double declaredLengthHostSamples =
                                (double) declaredLengthSteps * stepBeats * (60.0 / hostBpm) * hostSampleRate;
                            currentPickTapeStopDurationHostSamples = juce::jmin (declaredLengthHostSamples, samplesUntilNextActiveStep);
                            currentPickLengthInHostSamples = currentPickTapeStopDurationHostSamples; // only used for fadeIn clamping

#if JUCE_DEBUG
                            // TEMPORARY DEBUG -- remove once step-extension
                            // Tape Stop testing is done. Cheap atomic
                            // stores only (real-time-safe) -- the UI-thread
                            // drainDebugTapeStopEvents() does the actual
                            // DBG()/console I/O, off the audio thread. See
                            // the mailbox members' own doc comment in
                            // PluginProcessor.h for why this isn't a direct
                            // DBG() call here.
                            debugTapeStopPickStartRow.store (activeRow);
                            debugTapeStopPickStartStep.store (currentStepIndex);
                            debugTapeStopPickStartDeclaredLengthSteps.store (declaredLengthSteps);
                            debugTapeStopPickStartDeclaredLengthHostSamples.store (declaredLengthHostSamples);
                            debugTapeStopPickStartSamplesUntilNextActiveStep.store (samplesUntilNextActiveStep);
                            debugTapeStopPickStartDurationHostSamples.store (currentPickTapeStopDurationHostSamples);
                            debugTapeStopPickStartEventPending.store (true);
#endif
                        }
                        else if (stretch)
                        {
                            // Step-extension fix: Stretch's duration is now
                            // authoritative from the step's own declared
                            // length -- natural (Step-resolution-quantized)
                            // slice length, or its Step-extension override
                            // if Shift+drag set one -- exactly the same
                            // mechanism/precedence Tape Stop already uses
                            // (see its own branch above): the declared
                            // length is the ceiling, a genuinely upcoming
                            // step still cuts it off early. Grain Speed
                            // (currentPickStretchSpeedMultiplier) plays NO
                            // part in this -- it stays a fixed character
                            // constant (how fast ONE pass sweeps the
                            // slice), untouched by duration; if the
                            // declared length outlasts one pass, the SAME
                            // pass repeats to fill the remainder instead
                            // (see grainPlaybackStyle's loop mode in the
                            // granular render section below).
                            const int stretchDeclaredLengthSteps = getSequencerCellDeclaredLengthSteps (activeRow, currentStepIndex);
                            const double stretchDeclaredLengthHostSamples =
                                (double) stretchDeclaredLengthSteps * stepBeats * (60.0 / hostBpm) * hostSampleRate;
                            currentPickTapeStopDurationHostSamples = naturalLengthHostSamples; // unused, harmless
                            currentPickLengthInHostSamples = juce::jmin (stretchDeclaredLengthHostSamples, samplesUntilNextActiveStep);

#if JUCE_DEBUG
                            // TEMPORARY DEBUG -- remove once Stretch
                            // step-extension investigation is done. Same
                            // cheap-atomic-stores-only pattern as the Tape
                            // Stop pick-start log -- see its own doc
                            // comment for why this isn't a direct DBG()
                            // call here. Mirrors Tape Stop's own pick-start
                            // log shape now that the calculation itself
                            // does too; speedMultiplier/passLengthHostSamples
                            // are still logged for context even though
                            // neither drives duration -- passLength vs
                            // finalLength shows whether this particular
                            // pick actually needed the loop-to-repeat
                            // behaviour or was covered by one pass.
                            debugStretchPickStartRow.store (activeRow);
                            debugStretchPickStartStep.store (currentStepIndex);
                            debugStretchPickStartDeclaredLengthSteps.store (stretchDeclaredLengthSteps);
                            debugStretchPickStartDeclaredLengthHostSamples.store (stretchDeclaredLengthHostSamples);
                            debugStretchPickStartNaturalLengthHostSamples.store (naturalLengthHostSamples);
                            debugStretchPickStartSpeedMultiplier.store ((double) currentPickStretchSpeedMultiplier);
                            debugStretchPickStartPassLengthHostSamples.store ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples);
                            debugStretchPickStartSamplesUntilNextActiveStep.store (samplesUntilNextActiveStep);
                            debugStretchPickStartFinalLengthHostSamples.store (currentPickLengthInHostSamples);
                            debugStretchPickStartEventPending.store (true);
#endif
                        }
                        else
                        {
                            // Step-extension fix: Forward/Ping-Pong/Filter
                            // Down/Up's duration is now authoritative from
                            // the step's own declared length, same
                            // mechanism/precedence Tape Stop and Stretch
                            // above already use -- the declared length is
                            // the ceiling, a genuinely upcoming step still
                            // cuts it off early (samplesUntilNextActiveStep,
                            // same "whichever comes first" pattern as
                            // Step 43's original schedule-clamping, which
                            // this supersedes).
                            //
                            // When the declared length exceeds the natural
                            // unit -- one raw slice playthrough for Forward
                            // and Filter Down/Up, one round trip for
                            // Ping-Pong (its half-content window, see
                            // pingPongFoldLengthSamples below, makes one
                            // round trip exactly naturalLengthHostSamples
                            // long here) -- that unit LOOPS to fill the
                            // remainder, the same "repeat the natural unit"
                            // principle already built for Stretch, just
                            // with the raw slice/round-trip as the
                            // repeating unit instead of a stretched pass.
                            // No extra code needed for the loop itself:
                            // grainPlaybackStyle (loop for Forward/Filter
                            // Down/Up, the existing pingPong fold for
                            // Ping-Pong -- see below) already wraps
                            // unconditionally, a no-op whenever this
                            // duration never actually exceeds one natural
                            // unit in the first place. Filter Down/Up's
                            // sweep timeline (currentWindowLengthHostSamples,
                            // set below from this same value, BEFORE
                            // Subdivide slices currentPickLengthInHostSamples
                            // into individual ticks) rides along for free
                            // too -- one continuous glide across the whole
                            // declared length regardless of how many times
                            // the underlying audio loops underneath it, or
                            // how many Subdivide retriggers happen, exactly
                            // the same Whole Window principle Clock mode's
                            // own scope setting already proved.
                            const int declaredLengthSteps = getSequencerCellDeclaredLengthSteps (activeRow, currentStepIndex);
                            const double declaredLengthHostSamples =
                                (double) declaredLengthSteps * stepBeats * (60.0 / hostBpm) * hostSampleRate;
                            currentPickTapeStopDurationHostSamples = naturalLengthHostSamples; // unused, harmless
                            currentPickLengthInHostSamples = juce::jmin (declaredLengthHostSamples, samplesUntilNextActiveStep);

#if JUCE_DEBUG
                            // TEMPORARY DEBUG -- remove once Forward/
                            // Ping-Pong/Filter Down/Up step-extension
                            // verification is done. Same cheap-atomic-
                            // stores-only pattern as the Tape Stop/Stretch
                            // pick-start logs above.
                            debugLoopStylePickStartRow.store (activeRow);
                            debugLoopStylePickStartStep.store (currentStepIndex);
                            debugLoopStylePickStartStyleIndex.store ((int) currentPlaybackStyle);
                            debugLoopStylePickStartDeclaredLengthSteps.store (declaredLengthSteps);
                            debugLoopStylePickStartDeclaredLengthHostSamples.store (declaredLengthHostSamples);
                            debugLoopStylePickStartNaturalLengthHostSamples.store (naturalLengthHostSamples);
                            debugLoopStylePickStartSamplesUntilNextActiveStep.store (samplesUntilNextActiveStep);
                            debugLoopStylePickStartFinalLengthHostSamples.store (currentPickLengthInHostSamples);
                            debugLoopStylePickStartEventPending.store (true);
#endif
                        }

                        // Subdivide (Step 47): per-step retrigger rate,
                        // general -- offered (and read) regardless of this
                        // step's own PlaybackStyle, unlike Resonance/Filter
                        // Type/Curve Shape/Grain Size/Grain Speed above.
                        // 0 (Off, the default/fallback) leaves everything
                        // exactly as it was above this point.
                        //
                        // "Whole Window" reuse for Filter Down/Up (Step 30's
                        // Filter Sweep scope, generalized here): this step's
                        // OWN total duration -- exactly what was just
                        // computed above, BEFORE Subdivide slices it into
                        // individual retrigger ticks below -- becomes the
                        // window samplesSinceWindowStart/currentWindowLength-
                        // HostSamples track, the same pair Clock mode's own
                        // Whole Window scope already uses (see useWholeWindow
                        // further down). Set unconditionally (not just when
                        // Subdivide is on): when it's off there's only ever
                        // one trigger occupying the whole step anyway, so
                        // Whole Window and Per Tick are numerically identical
                        // in that case -- no behaviour change for existing,
                        // non-subdivided steps.
                        const double stepWindowLengthHostSamples =
                            tapeStop ? currentPickTapeStopDurationHostSamples : currentPickLengthInHostSamples;
                        samplesSinceWindowStart = 0.0;
                        currentWindowLengthHostSamples = stepWindowLengthHostSamples;

                        const int subdivideOption = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Subdivide", 0.0f));
                        sequencedSubdivisionActive = subdivideOption > 0;
                        sequencedSubdivisionRow = activeRow;

                        if (sequencedSubdivisionActive)
                        {
                            const double subdivisionBeats = getNoteValueBeats (subdivideOption - 1);
                            sequencedSubdivisionTickLengthHostSamples =
                                juce::jmax (1.0, subdivisionBeats * (60.0 / hostBpm) * hostSampleRate);
                            sequencedNextSubdivisionOffsetHostSamples = sequencedSubdivisionTickLengthHostSamples;

                            // This step's own first trigger (just started
                            // above) IS the first subdivision slot -- cap its
                            // length/duration to one tick, same jmin pattern
                            // Clock mode's own ticks already use, so its
                            // fade-out completes before the next retrigger
                            // rather than spilling into it.
                            currentPickLengthInHostSamples =
                                juce::jmin (currentPickLengthInHostSamples, sequencedSubdivisionTickLengthHostSamples);

                            if (tapeStop)
                                currentPickTapeStopDurationHostSamples =
                                    juce::jmin (currentPickTapeStopDurationHostSamples, sequencedSubdivisionTickLengthHostSamples);
                        }
                    }
                }

                // Subdivide retrigger (Step 47): runs every SAMPLE,
                // independent of the step-boundary check above -- same
                // per-sample discipline as everything else here, so a
                // retrigger point landing mid-block is never missed.
                // samplesSinceWindowStart/currentWindowLengthHostSamples are
                // the exact same pair set at this note's own pick-start
                // above (and, in Clock mode, already drive the Whole Window
                // Filter Sweep) -- reusing them here for scheduling too
                // keeps a subdivided step's retrigger grid and its Filter
                // Down/Up sweep locked to the same clock, and needs no
                // separate ppq-based scheduler.
                if (sequencedSubdivisionActive
                    && sequencedSubdivisionRow >= 0 && sequencedSubdivisionRow < (int) slices.size()
                    && samplesSinceWindowStart < currentWindowLengthHostSamples
                    && samplesSinceWindowStart >= sequencedNextSubdivisionOffsetHostSamples)
                {
                    const auto& subdivisionSlice = slices[(size_t) sequencedSubdivisionRow];
                    currentPosition = (double) subdivisionSlice.startSample;
                    samplesSincePickStart = 0.0;
                    hasCurrentPick = true;
                    pickJustStarted = true;

                    const double remainingWindowHostSamples =
                        juce::jmax (0.0, currentWindowLengthHostSamples - samplesSinceWindowStart);
                    currentPickLengthInHostSamples =
                        juce::jmin (sequencedSubdivisionTickLengthHostSamples, remainingWindowHostSamples);

                    if (currentPlaybackStyle == PlaybackStyle::tapeStop)
                        currentPickTapeStopDurationHostSamples = currentPickLengthInHostSamples;

                    sequencedNextSubdivisionOffsetHostSamples += sequencedSubdivisionTickLengthHostSamples;
                }
            }
        }
        else if (performanceMode)
        {
            // Quantize Recall: mirrors Pattern Switch Timing's Set Interval
            // scheme above (see the sequencedMode branch's own boundary
            // check), reusing the same per-sample host-ppq
            // boundary-crossing detection -- checked every SAMPLE, not once
            // per block, the same discipline that avoids the Step 6 bug (a
            // boundary computed once per block silently missing one that
            // lands mid-block). A pending recall (pendingPerformanceRecallNote,
            // armed by handlePerformanceStateNoteOn() while Quantize Recall
            // is on and the transport was playing at note-on time) is
            // applied the instant its chosen grid point is reached; a newer
            // note-on before that point just overwrites the same atomic
            // there, so the newest press always wins.
            //
            // Falls back to applying immediately, right here, if the host
            // transport isn't playing -- there's no meaningful beat position
            // to quantize against without it, and this also rescues a
            // recall that was armed while playing but whose target transport
            // then stopped before the boundary was ever reached (ppq holds
            // still while stopped, so an unplayed boundary would otherwise
            // never arrive) -- both preserve the "auditionable without
            // pressing play" behaviour Performance mode already has.
            const int pendingRecallNote = pendingPerformanceRecallNote.load();

            if (pendingRecallNote >= 0)
            {
                bool boundaryCrossed = ! hostTransportPlaying;

                if (hostTransportPlaying)
                {
                    if (! performanceQuantizeRecallBoundaryArmed)
                    {
                        // Just (re-)armed -- snap to the next grid point
                        // from wherever ppq is right now, same
                        // "windowIndex + 1" shape Clock mode/Reset/Set
                        // Interval all use to find their own next boundary.
                        const double intervalBeats = juce::jmax (
                            getNoteValueBeats (performanceQuantizeRecallIntervalIndex.load()), 1.0e-6);
                        const juce::int64 intervalIndex = (juce::int64) std::floor (samplePpq / intervalBeats);
                        performanceQuantizeRecallBoundaryPpq = (double) (intervalIndex + 1) * intervalBeats;
                        performanceQuantizeRecallBoundaryArmed = true;
                    }

                    boundaryCrossed = (samplePpq >= performanceQuantizeRecallBoundaryPpq);
                }

                if (boundaryCrossed)
                {
                    applyPerformanceStateRecall (pendingRecallNote);
                    pendingPerformanceRecallNote.store (-1);
                    performanceQuantizeRecallBoundaryArmed = false;
                }
            }

            // Performance mode: no probability engine, no slices -- one
            // hand-trimmed segment, one style, that style's own independent
            // parameter values, driven purely by MIDI note-on
            // (performanceRecallPending, set by handlePerformanceStateNoteOn()/
            // applyPerformanceStateRecall() in dispatchNoteOn()'s routing, or
            // by the Quantize Recall boundary check just above) rather than
            // by anything time/position-based. Physical MIDI is playback-only
            // (the on-screen keyboard is the only thing that ever changes
            // focus -- see setFocusedPerformanceStateSlot()): pressing the
            // FOCUSED slot's key plays performanceWorkingState itself (live
            // in-progress edits, via the shared trim atomics every other
            // mode also edits through); pressing any OTHER slot's key plays
            // a frozen copy of ITS saved snapshot
            // (currentlyPlayingPerformanceSnapshot, including its own saved
            // trim), captured at note-on time so auditioning it never
            // disturbs performanceWorkingState or focus. Starts and stays
            // silent (hasCurrentPick == false) until the first note-on this
            // session -- see the performanceMode block-level init above,
            // which is the one place that sets it false; nothing here does.
            bool needsFreshPick = performanceRecallPending;
            performanceRecallPending = false;

            // Whichever source governs the CURRENTLY sounding (or about to
            // start) pick -- resolved once here so every reference below,
            // whether checking for completion or starting a fresh pick,
            // agrees on the same one. Follows performancePlaybackIsFocused,
            // set once at the note-on that actually started this pick (see
            // handlePerformanceStateNoteOn()), not re-derived from whatever
            // has focus right now -- so a focus change made from the UI
            // thread while a non-focused slot's pick is still sounding can
            // never retroactively swap what that pick is playing out from
            // under it.
            const PerformanceStateSnapshot& performanceActiveState = performancePlaybackIsFocused
                ? performanceWorkingState : currentlyPlayingPerformanceSnapshot;

            if (! needsFreshPick && hasCurrentPick)
            {
                // Same per-style "has this pick run its course" condition
                // Slice Length's own while-loop uses just below -- re-derived
                // here, same convention this function already follows per
                // mode, rather than shared, since each mode's surrounding
                // context (loop vs. one-shot, retry-on-empty-slice vs. not)
                // differs enough that sharing it would need its own
                // indirection to paper over those differences.
                const bool pickFinished = (currentPlaybackStyle == PlaybackStyle::tapeStop)
                    ? (samplesSincePickStart >= currentPickTapeStopDurationHostSamples)
                    : (currentPosition >= (double) (((currentPlaybackStyle == PlaybackStyle::pingPong
                                                           || currentPlaybackStyle == PlaybackStyle::stretch
                                                           || currentPlaybackStyle == PlaybackStyle::scratch)
                                                          ? currentEndSample
                                                          : juce::jmin (currentEndSample, sourceLength)) - 1));

                if (pickFinished)
                {
                    if (performanceActiveState.loop)
                        needsFreshPick = true; // Loop on: rechain the SAME segment+style, Slice Length's own "finishing is the cue" mechanic
                    else
                        hasCurrentPick = false; // Loop off: go silent and stay silent until the next note-on retriggers it
                }
            }

            if (needsFreshPick && performanceActiveState.populated)
            {
                currentPlaybackStyle = indexToPlaybackStyle (performanceActiveState.style);
                const bool pingPong = (currentPlaybackStyle == PlaybackStyle::pingPong);
                const bool stretch = (currentPlaybackStyle == PlaybackStyle::stretch);
                const bool scratch = (currentPlaybackStyle == PlaybackStyle::scratch);

                // Sync toggle: on follows whichever global Pitch Mode is
                // currently selected (playbackRate, unchanged); off forces
                // native/unsynced playback for this pick. Captured once
                // here, consulted uniformly at every downstream render site
                // via currentPickNativeRateActive/effectivePickPlaybackRate,
                // rather than re-checked inline at each one.
                currentPickNativeRateActive = ! performanceActiveState.sync;
                const double perfPlaybackRate = currentPickNativeRateActive
                    ? (sampleSampleRate / hostSampleRate) : playbackRate;

                // Own independent parameter storage -- indexed exactly as
                // getSequencerCellParameterName() documents, NEVER the
                // global default atomics Slice Length/Clock read (those
                // three lines above) or Sequenced mode's per-cell overrides.
                currentPickStretchGrainSizeMs = performanceActiveState.parameterValues[3];
                currentPickStretchSpeedMultiplier = performanceActiveState.parameterValues[4];

                // The focused slot's segment IS the shared trim atomics
                // (edited live via the same waveform trim handles every
                // other mode uses); any OTHER slot's segment is its own
                // saved trim, captured independently in its snapshot --
                // never the shared atomics, which stay owned by whichever
                // slot actually has focus.
                const int performanceSegmentStart = performancePlaybackIsFocused
                    ? trimStartSample.load() : performanceActiveState.trimStartSample;
                const int performanceSegmentEnd = performancePlaybackIsFocused
                    ? trimEndSample.load() : performanceActiveState.trimEndSample;

                currentSliceStartSample = performanceSegmentStart;
                currentSliceLength = performanceSegmentEnd - performanceSegmentStart;
                currentPosition = (double) currentSliceStartSample;

                currentPickScratchCycleLengthHostSamples = scratch
                    ? computeScratchCycleLengthHostSamples (juce::roundToInt (performanceActiveState.parameterValues[10]),
                                                             currentSliceLength, hostBpm, hostSampleRate, perfPlaybackRate)
                    : 0.0;

                currentPickScratchForwardCurve = easingCurveFromIndex (juce::roundToInt (performanceActiveState.parameterValues[11]));
                currentPickScratchBackwardCurve = easingCurveFromIndex (juce::roundToInt (performanceActiveState.parameterValues[12]));

                currentEndSample = pingPong ? (2 * (currentSliceStartSample + currentSliceLength) - currentSliceStartSample)
                                 : stretch ? (int) (currentSliceStartSample + (double) currentPickStretchSpeedMultiplier * currentSliceLength)
                                 : scratch ? (int) (currentSliceStartSample + currentPickScratchCycleLengthHostSamples * perfPlaybackRate)
                                           : (currentSliceStartSample + currentSliceLength);

                hasCurrentPick = true;
                pickJustStarted = true;
                currentlyPlayingSliceIndexForUI.store (-1); // this pick isn't drawn from `slices` -- no index to report

                currentPickBeatQuantized = false; // Performance mode's own Sync toggle governs rate instead (currentPickNativeRateActive above)

                currentPickFilterSweepResonance = performanceActiveState.parameterValues[0];
                currentPickFilterSweepType = juce::roundToInt (performanceActiveState.parameterValues[1]);
                currentPickCurveShape = juce::roundToInt (performanceActiveState.parameterValues[2]);

                currentPickBitcrushRateValue = performanceActiveState.parameterValues[6];
                currentPickBitcrushRateMode = juce::roundToInt (performanceActiveState.parameterValues[7]);
                currentPickBitcrushBitDepthValue = performanceActiveState.parameterValues[8];
                currentPickBitcrushBitDepthMode = juce::roundToInt (performanceActiveState.parameterValues[9]);

                currentPickFlangerDelayValue = performanceActiveState.parameterValues[13];
                currentPickFlangerDelayMode = juce::roundToInt (performanceActiveState.parameterValues[14]);
                currentPickFlangerMixValue = performanceActiveState.parameterValues[15];
                currentPickFlangerMixMode = juce::roundToInt (performanceActiveState.parameterValues[16]);
                currentPickFlangerFeedbackValue = performanceActiveState.parameterValues[17];
                currentPickFlangerFeedbackMode = juce::roundToInt (performanceActiveState.parameterValues[18]);

                samplesSincePickStart = 0.0;
                const double naturalLengthHostSamples =
                    (perfPlaybackRate > 0.0) ? ((double) currentSliceLength / perfPlaybackRate) : 0.0;
                currentPickMidpointHostSamples = scratch
                    ? (currentPickScratchCycleLengthHostSamples * 0.5) : naturalLengthHostSamples;
                currentPickTapeStopDurationHostSamples = naturalLengthHostSamples;
                currentPickLengthInHostSamples = pingPong ? (2.0 * naturalLengthHostSamples)
                                                : stretch ? ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples)
                                                : scratch ? currentPickScratchCycleLengthHostSamples
                                                          : naturalLengthHostSamples;
            }
        }
        else if (controlMode)
        {
            // Control mode: a slice note-on (handleControlNoteOn(), routed
            // through dispatchNoteOn() before this per-sample loop runs)
            // triggers whichever slice it resolved to, at whichever
            // PlaybackStyle the last keyswitch selected, at a gain derived
            // from that note-on's own velocity. No probability engine, no
            // window/tick concept -- transport-independent, exactly like
            // Performance mode just above -- driven purely by
            // controlNoteOnPending, consumed here the same way
            // performanceRecallPending is consumed there.
            bool needsFreshPick = controlNoteOnPending;
            controlNoteOnPending = false;

            if (! needsFreshPick && hasCurrentPick)
            {
                // Same per-style "has this pick run its course" condition
                // every other mode's own per-sample dispatch re-derives.
                // Trigger mode (the default) always lets a pick run to its
                // own natural/effect-driven completion -- identical to
                // every other mode here; Gate mode's early stop is handled
                // entirely by the release-gain countdown in the shared gain
                // stage below, which forces hasCurrentPick false on its own
                // once the release fade completes, so there's nothing extra
                // to check for Gate here.
                const bool pickFinished = (currentPlaybackStyle == PlaybackStyle::tapeStop)
                    ? (samplesSincePickStart >= currentPickTapeStopDurationHostSamples)
                    : (currentPosition >= (double) (((currentPlaybackStyle == PlaybackStyle::pingPong
                                                           || currentPlaybackStyle == PlaybackStyle::stretch
                                                           || currentPlaybackStyle == PlaybackStyle::scratch)
                                                          ? currentEndSample
                                                          : juce::jmin (currentEndSample, sourceLength)) - 1));

                if (pickFinished)
                {
                    hasCurrentPick = false; // one-shot -- no loop concept in Control mode; stays silent until the next note-on
                    controlGateReleaseActive = false; // nothing left to release-fade
                }
            }

            if (needsFreshPick && controlPendingSliceIndex >= 0 && controlPendingSliceIndex < (int) slices.size())
            {
                controlGateReleaseActive = false; // monophonic retrigger abandons any release fade already in progress
                controlGateReleaseElapsedSamples = 0.0;
                controlCurrentlySoundingNote = controlPendingNoteNumber;
                currentControlVelocityGain = controlPendingVelocityGain;

                currentPlaybackStyle = indexToPlaybackStyle (controlPendingStyle);
                const bool pingPong = (currentPlaybackStyle == PlaybackStyle::pingPong);
                const bool stretch = (currentPlaybackStyle == PlaybackStyle::stretch);
                const bool scratch = (currentPlaybackStyle == PlaybackStyle::scratch);

                // Control mode always uses the global default parameter
                // values -- same "no per-note override" convention Clock/
                // Slice Length modes already follow (Sequenced mode is the
                // only one with per-cell overrides).
                currentPickStretchGrainSizeMs = stretchGrainSizeMsValue.load();
                currentPickStretchSpeedMultiplier = stretchSpeedMultiplierValue.load();

                const auto& slice = slices[(size_t) controlPendingSliceIndex];
                currentSliceStartSample = slice.startSample;
                currentSliceLength = slice.endSample - slice.startSample;
                currentPosition = (double) slice.startSample;

                currentPickScratchCycleLengthHostSamples = scratch
                    ? computeScratchCycleLengthHostSamples (getScratchRateGlobal(), currentSliceLength,
                                                             hostBpm, hostSampleRate, playbackRate)
                    : 0.0;

                currentPickScratchForwardCurve = easingCurveFromIndex (getScratchForwardCurveGlobal());
                currentPickScratchBackwardCurve = easingCurveFromIndex (getScratchBackwardCurveGlobal());

                currentEndSample = pingPong ? (2 * slice.endSample - slice.startSample)
                                 : stretch ? (int) (slice.startSample + (double) currentPickStretchSpeedMultiplier * currentSliceLength)
                                 : scratch ? (int) (slice.startSample + currentPickScratchCycleLengthHostSamples * playbackRate)
                                           : slice.endSample;
                hasCurrentPick = true;
                pickJustStarted = true;
                currentlyPlayingSliceIndexForUI.store (controlPendingSliceIndex);

                currentPickBeatQuantized = false; // no window/tick concept here to quantize against, same as Performance mode
                currentPickNativeRateActive = false;

                currentPickFilterSweepResonance = filterSweepResonanceValue.load();
                currentPickFilterSweepType = filterSweepFilterTypeValue.load();
                currentPickCurveShape = curveShapeValue.load();

                currentPickBitcrushRateValue = getBitcrushRateReductionGlobal();
                currentPickBitcrushRateMode = getBitcrushRateReductionModeGlobal();
                currentPickBitcrushBitDepthValue = getBitcrushBitDepthGlobal();
                currentPickBitcrushBitDepthMode = getBitcrushBitDepthModeGlobal();

                currentPickFlangerDelayValue = getFlangerDelayTimeGlobal();
                currentPickFlangerDelayMode = getFlangerDelayTimeModeGlobal();
                currentPickFlangerMixValue = getFlangerMixGlobal();
                currentPickFlangerMixMode = getFlangerMixModeGlobal();
                currentPickFlangerFeedbackValue = getFlangerFeedbackGlobal();
                currentPickFlangerFeedbackMode = getFlangerFeedbackModeGlobal();

                samplesSincePickStart = 0.0;
                const double naturalLengthHostSamples = (playbackRate > 0.0) ? ((double) currentSliceLength / playbackRate) : 0.0;
                currentPickMidpointHostSamples = scratch
                    ? (currentPickScratchCycleLengthHostSamples * 0.5)
                    : naturalLengthHostSamples;
                currentPickTapeStopDurationHostSamples = naturalLengthHostSamples;
                currentPickLengthInHostSamples = pingPong ? (2.0 * naturalLengthHostSamples)
                                                : stretch ? ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples)
                                                : scratch ? currentPickScratchCycleLengthHostSamples
                                                          : naturalLengthHostSamples;
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
                                                                 || currentPlaybackStyle == PlaybackStyle::stretch
                                                                 || currentPlaybackStyle == PlaybackStyle::scratch)
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
                const bool scratch = (currentPlaybackStyle == PlaybackStyle::scratch);

                // Stretch grain settings (Step 46) -- Slice Length mode
                // always uses the global values; per-step overrides are
                // Sequenced-mode-only. Captured here, before currentEndSample
                // below, since Grain Speed feeds directly into that
                // calculation.
                currentPickStretchGrainSizeMs = stretchGrainSizeMsValue.load();
                currentPickStretchSpeedMultiplier = stretchSpeedMultiplierValue.load();

                const auto& slice = slices[(size_t) currentSliceIndex];
                currentSliceStartSample = slice.startSample;
                currentSliceLength = slice.endSample - slice.startSample;
                currentPosition = (double) slice.startSample;

                // Scratch (v1) -- Slice Length mode always uses the global
                // Rate value; per-step overrides are Sequenced-mode-only,
                // same pattern as Bitcrush above. Captured here, before
                // currentEndSample below, since it feeds directly into that
                // calculation, same as Grain Speed does for Stretch.
                currentPickScratchCycleLengthHostSamples = scratch
                    ? computeScratchCycleLengthHostSamples (getScratchRateGlobal(), currentSliceLength,
                                                             hostBpm, hostSampleRate, playbackRate)
                    : 0.0;

                // Forward/Backward Curve (Scratch v2) -- Slice Length mode
                // always uses the global values; per-step overrides are
                // Sequenced-mode-only, same pattern as Rate just above.
                currentPickScratchForwardCurve = easingCurveFromIndex (getScratchForwardCurveGlobal());
                currentPickScratchBackwardCurve = easingCurveFromIndex (getScratchBackwardCurveGlobal());

                currentEndSample = pingPong ? (2 * slice.endSample - slice.startSample)
                                 : stretch ? (int) (slice.startSample + (double) currentPickStretchSpeedMultiplier * currentSliceLength)
                                 : scratch ? (int) (slice.startSample + currentPickScratchCycleLengthHostSamples * playbackRate)
                                           : slice.endSample;
                hasCurrentPick = true;
                pickJustStarted = true;
                currentlyPlayingSliceIndexForUI.store (currentSliceIndex);

                // Filter Sweep resonance/filter type + Curve Shape (Step
                // 45/46) -- Slice Length mode always uses the global
                // values; per-step overrides are Sequenced-mode-only.
                currentPickFilterSweepResonance = filterSweepResonanceValue.load();
                currentPickFilterSweepType = filterSweepFilterTypeValue.load();
                currentPickCurveShape = curveShapeValue.load();

                // Bitcrush Sample Rate Reduction/Bit Depth (Step 49) --
                // Slice Length mode always uses the global values;
                // per-step value/mode overrides are Sequenced-mode-only,
                // same pattern as Filter Sweep above.
                currentPickBitcrushRateValue = getBitcrushRateReductionGlobal();
                currentPickBitcrushRateMode = getBitcrushRateReductionModeGlobal();
                currentPickBitcrushBitDepthValue = getBitcrushBitDepthGlobal();
                currentPickBitcrushBitDepthMode = getBitcrushBitDepthModeGlobal();

                // Flanger Delay Time/Mix/Feedback -- Slice Length mode
                // always uses the global values; per-step value/mode
                // overrides are Sequenced-mode-only, same pattern as
                // Bitcrush just above.
                currentPickFlangerDelayValue = getFlangerDelayTimeGlobal();
                currentPickFlangerDelayMode = getFlangerDelayTimeModeGlobal();
                currentPickFlangerMixValue = getFlangerMixGlobal();
                currentPickFlangerMixMode = getFlangerMixModeGlobal();
                currentPickFlangerFeedbackValue = getFlangerFeedbackGlobal();
                currentPickFlangerFeedbackMode = getFlangerFeedbackModeGlobal();

                samplesSincePickStart = 0.0;
                const double naturalLengthHostSamples = (playbackRate > 0.0) ? ((double) currentSliceLength / playbackRate) : 0.0;
                currentPickMidpointHostSamples = scratch
                    ? (currentPickScratchCycleLengthHostSamples * 0.5)
                    : naturalLengthHostSamples; // where a Ping-Pong round trip reverses; unused for Forward. Scratch (v1) uses half its OWN cycle length instead.
                currentPickTapeStopDurationHostSamples = naturalLengthHostSamples; // the pick's own natural length; unused for Forward/Ping-Pong/Stretch/Scratch
                currentPickLengthInHostSamples = pingPong ? (2.0 * naturalLengthHostSamples)
                                                : stretch ? ((double) currentPickStretchSpeedMultiplier * naturalLengthHostSamples)
                                                : scratch ? currentPickScratchCycleLengthHostSamples
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
                currentPickNativeRateActive = false; // Performance mode's Sync-off override never applies outside Performance mode's own picks

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
            filterSweepFilter.setResonance (currentPickFilterSweepResonance); // Step 45 -- this pick's own value (global, or Sequenced mode's per-step override)

            // Filter type (Step 46) -- same per-pick value/override
            // treatment as resonance just above.
            filterSweepFilter.setType (currentPickFilterSweepType == 1 ? juce::dsp::StateVariableTPTFilterType::highpass
                                      : currentPickFilterSweepType == 2 ? juce::dsp::StateVariableTPTFilterType::bandpass
                                                                        : juce::dsp::StateVariableTPTFilterType::lowpass);

            // Bitcrush (Step 48) -- no bleed from a previous pick, same
            // reasoning as filterSweepFilter.reset() above: a fresh pick
            // starts its sample-and-hold phase from scratch (counter at 0
            // grabs a fresh value on this pick's very first sample) rather
            // than continuing whatever hold phase/value the previous pick
            // (of any style) happened to leave behind.
            bitcrushHoldCounter = 0;
            std::fill (std::begin (bitcrushHeldSample), std::end (bitcrushHeldSample), 0.0f);

            // Flanger -- same "no bleed from a previous pick" reasoning as
            // bitcrushHoldCounter/bitcrushHeldSample just above: a fresh
            // pick's comb character starts from a silent delay line rather
            // than continuing whatever content the previous pick (of any
            // style) happened to leave behind.
            flangerDelayBuffer.clear();
            flangerDelayWriteIndex = 0;

#if JUCE_DEBUG
            // TEMPORARY DEBUG -- remove once step-extension Tape Stop
            // testing is done. Fresh pick, fresh edges to detect below --
            // a brand new pick always starts within its own schedule and
            // not yet position-exhausted, regardless of style (harmless
            // when the new pick isn't Tape Stop at all).
            debugTapeStopPrevWithinSchedule = true;
            debugTapeStopPrevExhausted = false;
            debugTapeStopPrevGainNearZero = false;
#endif
        }

        const bool pingPongActive = (currentPlaybackStyle == PlaybackStyle::pingPong);
        const bool tapeStopActive = (currentPlaybackStyle == PlaybackStyle::tapeStop);
        const bool stretchActive = (currentPlaybackStyle == PlaybackStyle::stretch);
        const bool filterSweepActive = (currentPlaybackStyle == PlaybackStyle::filterSweepDown
                                         || currentPlaybackStyle == PlaybackStyle::filterSweepUp);
        const bool bitcrushActive = (currentPlaybackStyle == PlaybackStyle::bitcrush);
        const bool scratchActive = (currentPlaybackStyle == PlaybackStyle::scratch);
        const bool flangerActive = (currentPlaybackStyle == PlaybackStyle::flanger);

        // Performance mode's Sync toggle (Pass 1): currentPickNativeRateActive
        // was captured once at this pick's own pick-start (false for every
        // other mode's picks) -- substitutes a native (unsynced) rate for
        // playbackRate at every downstream render site that would otherwise
        // read it directly, so a Sync-off Performance pick can never read as
        // synced at one site and unsynced at another.
        const double effectivePickPlaybackRate = currentPickNativeRateActive
            ? (sampleSampleRate / hostSampleRate) : playbackRate;

        // Scratch (v1) reuses Ping-Pong's exact bounce mechanism -- same
        // foldPosition() fold, same turnaround click-avoidance fade, same
        // "position marches on unbounded, only folded at render time" gate
        // -- just with its own Rate-driven cycle length (bounceFoldLength-
        // Samples below) standing in for pingPongFoldLengthSamples, so
        // every place that mechanism is consulted below treats the two
        // styles identically via this one shared flag.
        const bool bounceActive = pingPongActive || scratchActive;

        // Bitcrush Sweep In/Out (Step 49): plain linear interpolation from
        // this pick's own set value toward (Sweep In) or away from (Sweep
        // Out) the parameter's fixed extreme, driven by the SAME
        // samplesSincePickStart/currentPickLengthInHostSamples progress
        // fraction the fade envelope and Filter Sweep's own Per Tick
        // formula already use below -- deliberately NOT Filter Sweep's
        // log-scale remap, since these are small linear integer counts
        // (sample-hold length, bit count), not a wide perceptually-
        // logarithmic frequency range. Reusing that one shared progress
        // source is also what makes this automatically span a step's full
        // EXTENDED duration for free when step-extension has stretched
        // currentPickLengthInHostSamples past the pick's natural length --
        // no separate step-extension handling needed here, same as base
        // Bitcrush already required none for its render-continue gate.
        double bitcrushProgress = 0.0;

        if (bitcrushActive)
        {
            bitcrushProgress = (currentPickLengthInHostSamples > 0.0)
                ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                : 1.0;
        }

        // mode: 0 Static (unchanged), 1 Sweep In (set value -> extreme),
        // 2 Sweep Out (extreme -> set value) -- see currentPickBitcrushRateMode/
        // currentPickBitcrushBitDepthMode's own doc comment in PluginProcessor.h.
        const auto sweptBitcrushValue = [bitcrushProgress] (float setValue, int mode, float extreme) -> float
        {
            if (mode == 1)
                return (float) (setValue + (extreme - setValue) * bitcrushProgress);

            if (mode == 2)
                return (float) (extreme + (setValue - extreme) * bitcrushProgress);

            return setValue;
        };

        const int effectiveBitcrushHoldSamples = bitcrushActive
            ? juce::jmax (1, juce::roundToInt (sweptBitcrushValue (currentPickBitcrushRateValue, currentPickBitcrushRateMode, bitcrushRateReductionExtreme)))
            : 1;
        const int effectiveBitcrushBitDepth = bitcrushActive
            ? juce::jlimit (1, 24, juce::roundToInt (sweptBitcrushValue (currentPickBitcrushBitDepthValue, currentPickBitcrushBitDepthMode, bitcrushBitDepthExtreme)))
            : 1;

        // Bitcrush sample-and-hold timing (Step 48/49): decided once per
        // OUTPUT sample here (not per channel below), so a stereo pair
        // holds/updates in lockstep rather than drifting apart -- counts
        // down to 0, grabbing a fresh value on the 0 tick and restarting
        // the count using WHATEVER the swept hold length is AT THAT
        // MOMENT (Step 49) -- Static's fixed length is just the mode==0
        // case of this same formula, so grab cadence smoothly follows a
        // Sweep In/Out's ramp rather than needing a separate code path.
        bool bitcrushShouldGrab = false;

        if (bitcrushActive)
        {
            if (bitcrushHoldCounter <= 0)
            {
                bitcrushShouldGrab = true;
                bitcrushHoldCounter = effectiveBitcrushHoldSamples - 1;
            }
            else
            {
                --bitcrushHoldCounter;
            }
        }

        // Bitcrush post-processing (Step 48): applied in the SAME slot as
        // Filter Sweep's cutoff filter below -- after the fade-gain
        // calculation, right before the sample is written to the output
        // buffer -- so it's a no-op for every other style and, like Filter
        // Sweep, never needs to know how its input sample was generated
        // (granular grain sum or direct-read interpolation) or how long
        // this pick/step will run. Bit-depth quantization happens once per
        // hold period, on the freshly grabbed sample (at THAT sample's own
        // effective bit depth, Step 49), and that quantized value is what
        // repeats for the rest of the period -- the classic stair-stepped
        // bitcrusher structure, not two independent effects.
        const auto applyBitcrush = [this, bitcrushShouldGrab, effectiveBitcrushBitDepth] (int channel, float rawSample) -> float
        {
            const int ch = juce::jmin (channel, GranularStretcher::maxChannels - 1);

            if (bitcrushShouldGrab)
            {
                const float quantStep = 2.0f / (float) (1 << effectiveBitcrushBitDepth);
                bitcrushHeldSample[ch] = quantStep * std::floor (rawSample / quantStep + 0.5f);
            }

            return bitcrushHeldSample[ch];
        };

        // Flanger Sweep In/Out: plain linear interpolation from this
        // pick's own set value toward/away from the parameter's fixed
        // extreme (same interpolation shape as Bitcrush's own Sweep
        // In/Out above), but driven by Filter Sweep's Whole Window
        // progress fraction rather than Bitcrush's per-pick one --
        // samplesSinceWindowStart/currentWindowLengthHostSamples,
        // continuous across an entire Clock-mode window or Sequenced-mode
        // step regardless of how many Subdivide retriggers happen
        // underneath, exactly the mechanism documented alongside
        // useWholeWindow further below. Unlike Filter Sweep, Flanger has
        // no Per Tick/Whole Window user toggle -- it always PREFERS whole-
        // window timing wherever a window exists (Clock mode
        // unconditionally; Sequenced mode whenever the active step has
        // Subdivide on), and only falls back to the per-pick formula where
        // there genuinely is no window to measure against (Slice Length
        // mode, and non-subdivided Sequenced steps -- numerically
        // identical to whole-window there anyway, per Filter Sweep's own
        // comment below, since only one trigger occupies the whole window/
        // step either way). This is what makes a Flanger step's sweep
        // glide once across the WHOLE step while Subdivide's retriggers
        // happen underneath, rather than resetting on every retrigger.
        const bool flangerUseWholeWindow = clockMode || (sequencedMode && sequencedSubdivisionActive);

        double flangerProgress = 0.0;

        if (flangerActive)
        {
            flangerProgress = flangerUseWholeWindow
                ? ((currentWindowLengthHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSinceWindowStart / currentWindowLengthHostSamples)
                       : 1.0)
                : ((currentPickLengthInHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                       : 1.0);
        }

        // mode: 0 Static (unchanged), 1 Sweep In (set value -> extreme),
        // 2 Sweep Out (extreme -> set value) -- see currentPickFlangerDelayMode/
        // currentPickFlangerMixMode's own doc comment in PluginProcessor.h.
        const auto sweptFlangerValue = [flangerProgress] (float setValue, int mode, float extreme) -> float
        {
            if (mode == 1)
                return (float) (setValue + (extreme - setValue) * flangerProgress);

            if (mode == 2)
                return (float) (extreme + (setValue - extreme) * flangerProgress);

            return setValue;
        };

        const int flangerDelayBufferLength = flangerDelayBuffer.getNumSamples();
        const int effectiveFlangerDelaySamples = (flangerActive && flangerDelayBufferLength > 2)
            ? juce::jlimit (1, flangerDelayBufferLength - 2, juce::roundToInt (
                  (sweptFlangerValue (currentPickFlangerDelayValue, currentPickFlangerDelayMode, flangerDelayTimeExtremeMs) / 1000.0) * hostSampleRate))
            : 1;
        const float effectiveFlangerMix = flangerActive
            ? juce::jlimit (0.0f, 1.0f, sweptFlangerValue (currentPickFlangerMixValue, currentPickFlangerMixMode, flangerMixExtreme))
            : 0.0f;
        const float effectiveFlangerFeedback = flangerActive
            ? juce::jlimit (0.0f, flangerFeedbackExtreme, sweptFlangerValue (currentPickFlangerFeedbackValue, currentPickFlangerFeedbackMode, flangerFeedbackExtreme))
            : 0.0f;

        // Volume ramp: a style-independent gain multiplier, unlike
        // Bitcrush/Flanger above which only apply to their own style --
        // gated on `sequencedMode` alone (every PlaybackStyle, Forward
        // included), not a currentPlaybackStyle check, since it's a pure
        // gain stage layered onto whatever the active style's own DSP
        // already produced (see the fade-gain block below). Also gates
        // OUT Slice Length/Clock mode entirely: those trigger modes never
        // populate currentPickVolumeValue/Mode (Sequenced-mode-only, no
        // global dial -- see PluginProcessor.h), so without this gate a
        // value captured during a prior Sequenced-mode step could
        // otherwise bleed into playback after switching modes.
        const bool volumeRampActive = sequencedMode;

        // Same Whole Window progress source as Flanger's flangerUseWholeWindow/
        // flangerProgress just above -- prefers samplesSinceWindowStart/
        // currentWindowLengthHostSamples (continuous across this step's
        // Subdivide retriggers) whenever Subdivide is actually on, falling
        // back to the per-pick fraction otherwise (numerically identical
        // there anyway, since only one trigger occupies the whole step).
        // This is what makes "Subdivide to 16ths across a whole bar, with
        // Volume ramping smoothly across that entire bar" work.
        const bool volumeUseWholeWindow = sequencedMode && sequencedSubdivisionActive;

        double volumeProgress = 0.0;

        if (volumeRampActive)
        {
            volumeProgress = volumeUseWholeWindow
                ? ((currentWindowLengthHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSinceWindowStart / currentWindowLengthHostSamples)
                       : 1.0)
                : ((currentPickLengthInHostSamples > 0.0)
                       ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickLengthInHostSamples)
                       : 1.0);
        }

        // mode: 0 Static (constant at the set level), 1 Ramp Up (silence
        // -> set level), 2 Ramp Down (set level -> silence) -- directional
        // language rather than Sweep In/Out's, since volume has an
        // intuitive up/down sense the other swept parameters don't. Both
        // ramp toward/away from a fixed extreme of 0.0 (silence), the
        // Volume equivalent of Bitcrush/Flanger's own fixed sweep
        // extremes, rather than a second user-adjustable value.
        const auto sweptVolumeValue = [volumeProgress] (float setValue, int mode) -> float
        {
            if (mode == 1)
                return (float) (setValue * volumeProgress);

            if (mode == 2)
                return (float) (setValue * (1.0 - volumeProgress));

            return setValue;
        };

        const double volumeGain = volumeRampActive
            ? juce::jlimit (0.0, 1.0, (double) sweptVolumeValue (currentPickVolumeValue, currentPickVolumeMode))
            : 1.0;

        // Control mode: velocity->volume, a per-pick constant captured at
        // trigger time (see currentControlVelocityGain's own doc comment) --
        // gated on controlMode itself, not just "is it 1.0 right now," so a
        // stale non-1.0 value can never bleed into a different mode after
        // switching away from Control mode mid-pick.
        const double controlVelocityGain = controlMode ? currentControlVelocityGain : 1.0;

        // Gate release (Control mode only): an ADDITIONAL fadeOutMs-long
        // ramp to silence, independent of whichever style's own duration/
        // completion math governs currentPickLengthInHostSamples above --
        // this is what lets one mechanism gate-stop all 9 PlaybackStyles
        // identically without special-casing any of them (see
        // controlGateReleaseActive's own doc comment in PluginProcessor.h).
        // Only ever active while Control mode's Gate setting has actually
        // armed it (handleControlNoteOff()), so this is a bare no-op (1.0)
        // everywhere else, same as volumeGain's own mode gate above.
        const double controlGateReleaseGain = controlGateReleaseActive
            ? juce::jlimit (0.0, 1.0, 1.0 - controlGateReleaseElapsedSamples / juce::jmax (1.0, fadeOutSamplesRequested))
            : 1.0;

        // Read position decided once per OUTPUT sample here (not per
        // channel below), same "stereo pair stays in lockstep" reasoning
        // as Bitcrush's shared hold counter above -- both channels read
        // the delay line the same number of samples back, they just carry
        // different content there.
        const int flangerReadIndex = (flangerActive && flangerDelayBufferLength > 0)
            ? (((flangerDelayWriteIndex - effectiveFlangerDelaySamples) % flangerDelayBufferLength + flangerDelayBufferLength) % flangerDelayBufferLength)
            : 0;

        // Flanger post-processing: applied in the SAME slot as Bitcrush/
        // Filter Sweep just above -- after the fade-gain calculation,
        // right before the sample is written to the output buffer -- so
        // it's a no-op for every other style. The freshly rendered DRY
        // sample plus Feedback's own share of whatever the line held
        // effectiveFlangerDelaySamples ago is what gets written into the
        // delay line (the classic feedback comb-filter structure -- this
        // is what makes Feedback actually resonant rather than just a
        // one-shot echo), while the OUTPUT stays a plain wet/dry crossfade
        // between the dry sample and that same delayed value, independent
        // of Feedback. effectiveFlangerFeedback is already clamped to
        // flangerFeedbackExtreme (well short of 1.0), so this recursion is
        // always stable -- never a runaway buildup.
        const auto applyFlanger = [this, flangerReadIndex, effectiveFlangerMix, effectiveFlangerFeedback] (int channel, float drySample) -> float
        {
            const int ch = juce::jmin (channel, GranularStretcher::maxChannels - 1);
            const int bufLen = flangerDelayBuffer.getNumSamples();

            if (bufLen <= 0)
                return drySample;

            float* delayData = flangerDelayBuffer.getWritePointer (ch);
            const float delayed = delayData[flangerReadIndex];

            delayData[flangerDelayWriteIndex % bufLen] = drySample + effectiveFlangerFeedback * delayed;

            return drySample + effectiveFlangerMix * (delayed - drySample);
        };

        // Ping-Pong's currentEndSample is a full round trip (2x slice
        // length), Stretch's is stretchDurationMultiplier-x, and Scratch's
        // (v1) is its own Rate-driven cycle length — all three can
        // legitimately run past sourceLength when the slice sits at the
        // very end of the buffer. That's fine: the actual read position
        // below is always either the FOLDED one (Ping-Pong/Scratch) or
        // safely bounded to the true slice by GranularStretcher itself
        // (Stretch always renders through it), regardless. The sourceLength
        // clamp is only needed for Forward/Tape Stop, where the raw
        // (unfolded) position IS the read position.
        const bool extendedRangeActive = bounceActive || stretchActive;
        const int schedulingEndSample = extendedRangeActive ? currentEndSample : juce::jmin (currentEndSample, sourceLength);

        // Tape Stop position-exhaustion fix: a step-extended (or Whole
        // Window/long-tick Clock) decel can now legitimately ask for more
        // real time than the slice has actual source samples for -- the
        // ramping tapeStopRateMultiplier only covers HALF that time's
        // worth of source distance on average (a linear 1.0->0.0 ramp),
        // but even that can exceed the slice's own length once the
        // requested duration is more than roughly 2x its natural length.
        // Once currentPosition would run past the slice's real content,
        // freeze it right there (hold the last sample/grain position)
        // rather than either reading into whatever audio happens to follow
        // it in the buffer or, as this used to do, cutting the whole pick
        // to silence outright while the gain envelope was still very much
        // audible. See its use below (positionForRead, and the granular
        // path's grainSourceHopSamples override).
        const bool tapeStopPositionExhausted = tapeStopActive && currentPosition >= (double) (schedulingEndSample - 1);

        // Shared render step for both modes: only output a sample while
        // we're within the current pick's bounds. In Clock mode, once a
        // short slice naturally finishes before the next tick, this
        // condition goes false and we correctly render silence until the
        // next forced retrigger resets currentPosition. Tape Stop is
        // gated on its own decel timer instead (currentPickTapeStop-
        // DurationHostSamples/samplesSincePickStart) rather than position
        // -- see tapeStopPositionExhausted above for why position alone
        // can no longer be trusted to reach the timer's own end at the
        // same moment. Stretch is gated on currentPickLengthInHostSamples
        // the same way, always (harmless/numerically identical to the old
        // position-based gate for Clock/Slice Length modes' own Stretch,
        // whose currentEndSample still encodes the same multiplier-based
        // length those modes have always used -- position advancing at
        // the plain playbackRate reaches that boundary at exactly
        // currentPickLengthInHostSamples there too, so this was always a
        // like-for-like swap, not a behaviour change, there).
        //
        // Step-extension fix: Sequenced mode's Forward/Ping-Pong/Filter
        // Down/Up now need the same duration-based gate too -- their
        // duration is authoritative from the step's own declared length
        // there (see the pick-start branch above), decoupled from
        // currentEndSample/position entirely once a step is extended
        // past its natural unit. Scoped to sequencedMode specifically
        // (rather than applied unconditionally, the way Stretch's swap
        // safely was) since Clock mode's own tick-forced retriggering
        // makes the equivalence argument above murkier there -- this
        // avoids touching Clock/Slice Length mode behaviour at all,
        // which nothing here was asked to change.
        const bool pickWithinSchedule = tapeStopActive
            ? (samplesSincePickStart < currentPickTapeStopDurationHostSamples)
            : (stretchActive || sequencedMode)
                ? (samplesSincePickStart < currentPickLengthInHostSamples)
                : (currentPosition < (double) (schedulingEndSample - 1));

#if JUCE_DEBUG
        // TEMPORARY DEBUG -- remove once step-extension Tape Stop testing
        // is done. Edge-detected (once per occurrence, not every sample)
        // exactly as before: (a) the exact sample the position-exhaustion
        // freeze first kicks in for this pick, (b) the exact sample this
        // pick's render gate closes, i.e. samplesSincePickStart at the
        // moment rendering actually stops. Only the OUTPUT changed --
        // cheap atomic stores into the mailbox instead of calling DBG()
        // directly here; drainDebugTapeStopEvents() (UI thread) does the
        // actual printing. See the mailbox members' own doc comment in
        // PluginProcessor.h for why.
        if (tapeStopActive)
        {
            if (tapeStopPositionExhausted && ! debugTapeStopPrevExhausted)
            {
                debugTapeStopFreezeSamplesSincePickStart.store (samplesSincePickStart);
                debugTapeStopFreezeDurationHostSamples.store (currentPickTapeStopDurationHostSamples);
                debugTapeStopFreezePosition.store (currentPosition);
                debugTapeStopFreezeSchedulingEndSample.store (schedulingEndSample);
                debugTapeStopFreezeEventPending.store (true);
            }

            if (! pickWithinSchedule && debugTapeStopPrevWithinSchedule)
            {
                debugTapeStopStopSamplesSincePickStart.store (samplesSincePickStart);
                debugTapeStopStopDurationHostSamples.store (currentPickTapeStopDurationHostSamples);
                debugTapeStopStopEverFroze.store (debugTapeStopPrevExhausted || tapeStopPositionExhausted);
                debugTapeStopStopEventPending.store (true);
            }

            debugTapeStopPrevExhausted = tapeStopPositionExhausted;
            debugTapeStopPrevWithinSchedule = pickWithinSchedule;
        }
#endif

        if (hasCurrentPick && pickWithinSchedule)
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
            // from 1.0 to 0.0 across currentPickTapeStopDurationHostSamples
            // -- Linear (default) or Exponential per Curve Shape (Step 46,
            // see applyCurveShape() above), layered on top of whatever
            // Pitch Mode already produces below. Gain rides the SAME curve,
            // REPLACING (not stacking with) the normal fadeOutMs for this
            // style — if rate hit exactly 0 while gain stayed at full, the
            // engine would get stuck holding/repeating a single sample (a
            // buzz) instead of fading to silence. fadeInMs above is
            // unaffected.
            double tapeStopRateMultiplier = 1.0;

            if (tapeStopActive)
            {
                const double rawProgress = (currentPickTapeStopDurationHostSamples > 0.0)
                    ? juce::jlimit (0.0, 1.0, samplesSincePickStart / currentPickTapeStopDurationHostSamples)
                    : 1.0; // degenerate zero-length duration -- treat as already fully stopped
                const double progress = applyCurveShape (rawProgress, currentPickCurveShape);
                tapeStopRateMultiplier = 1.0 - progress;

                gain = juce::jmin (gain, tapeStopRateMultiplier);

#if JUCE_DEBUG
                // TEMPORARY DEBUG -- remove once step-extension Tape Stop
                // testing is done. Edge-detects the sample the FINAL gain
                // (what actually multiplies the output sample -- not just
                // tapeStopRateMultiplier alone, which fadeIn could in
                // principle also clamp below) first drops under
                // debugTapeStopGainNearZeroThreshold, so its
                // samplesSincePickStart can be checked against
                // currentPickTapeStopDurationHostSamples directly -- proves
                // (or disproves) whether the ramp's own ~zero point lines
                // up with the pick's full declared duration, independent
                // of whatever the render path/freeze logic is doing with
                // read position.
                const bool debugGainNearZeroNow = gain < debugTapeStopGainNearZeroThreshold;

                if (debugGainNearZeroNow && ! debugTapeStopPrevGainNearZero)
                {
                    debugTapeStopGainNearZeroSamplesSincePickStart.store (samplesSincePickStart);
                    debugTapeStopGainNearZeroDurationHostSamples.store (currentPickTapeStopDurationHostSamples);
                    debugTapeStopGainNearZeroGainValue.store (gain);
                    debugTapeStopGainNearZeroRateMultiplier.store (tapeStopRateMultiplier);
                    debugTapeStopGainNearZeroWasExhausted.store (tapeStopPositionExhausted);
                    debugTapeStopGainNearZeroWasGranular.store (timeStretchMode || stretchActive);
                    debugTapeStopGainNearZeroEventPending.store (true);
                }

                debugTapeStopPrevGainNearZero = debugGainNearZeroNow;
#endif
            }
            else
            {
                if (fadeOutSamples > 0.0 && samplesRemaining < fadeOutSamples)
                    gain = juce::jmin (gain, samplesRemaining / fadeOutSamples);

                // Ping-Pong (Step 19), and Scratch (v1) via the same
                // bounceActive flag: the bounce point isn't a true edit --
                // audio isn't symmetric around any given sample, so
                // reversing direction there clicks just like the pick's own
                // start/end would without a fade. Treat the midpoint as an
                // internal boundary and apply the SAME fadeInMs/fadeOutMs
                // envelope around it: fading out approaching it (mirrors
                // the pick's own end-fade) and back in leaving it (mirrors
                // the start-fade). Layered into the same overall `gain`, so
                // it wraps whichever render path below produced the dry
                // sample -- Repitch or Time-Stretch alike. Curve Shape
                // (Step 46) applies here too, sharing applyCurveShape()
                // with Tape Stop's decel above rather than a separate
                // formula -- Linear (default) is the original behaviour.
                // Only ONE midpoint is faded per pick here, same as
                // Ping-Pong's own existing step-extension behaviour -- a
                // multi-cycle extended Scratch step gets this treatment at
                // its first turnaround only, not every subsequent bounce
                // (per-turnaround fading is deferred to v2, same as
                // per-direction curve shaping).
                if (bounceActive)
                {
                    const double distanceBeforeMidpoint = currentPickMidpointHostSamples - samplesSincePickStart;
                    const double distanceAfterMidpoint = samplesSincePickStart - currentPickMidpointHostSamples;

                    if (distanceBeforeMidpoint >= 0.0 && distanceBeforeMidpoint < fadeOutSamples)
                        gain = juce::jmin (gain, applyCurveShape (distanceBeforeMidpoint / fadeOutSamples, currentPickCurveShape));

                    if (distanceAfterMidpoint >= 0.0 && distanceAfterMidpoint < fadeInSamples)
                        gain = juce::jmin (gain, applyCurveShape (distanceAfterMidpoint / fadeInSamples, currentPickCurveShape));
                }
            }

            gain = juce::jlimit (0.0, 1.0, gain);

            // Volume ramp: an ADDITIONAL multiplier on top of the base
            // fade-in/out gain just clamped above, not a replacement for
            // it -- Volume handles the musical gesture across the whole
            // step/window, the base fade still handles click-avoidance at
            // the pick's own hard start/end boundaries (and, for
            // Ping-Pong/Scratch, its midpoint). Both are already clamped
            // to [0, 1] individually, so multiplying them together stays
            // in range with no further clamp needed. A no-op (1.0) outside
            // Sequenced mode -- see volumeRampActive above.
            gain *= volumeGain;

            // Control mode: velocity->volume and Gate's release fade, same
            // "additional multiplier, already clamped to [0, 1]" shape as
            // Volume just above -- both are bare no-ops (1.0) outside
            // Control mode, see their own doc comments just above.
            gain *= controlVelocityGain;
            gain *= controlGateReleaseGain;

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
            // Sequenced mode's Subdivide (Step 47) forces this same Whole
            // Window behaviour whenever the currently-playing step has a
            // subdivision rate set -- its own "window" is that one step's
            // total duration (see samplesSinceWindowStart/currentWindow-
            // LengthHostSamples set at that step's pick-start), so the
            // sweep glides continuously across the whole step while the
            // subdivision retriggers happen underneath, rather than each
            // retrigger getting its own reset sweep. A non-subdivided
            // Sequenced step still falls through to the Per Tick formula
            // below, but the two are numerically identical there (only one
            // trigger occupies the whole step either way), so this changes
            // nothing for existing, non-subdivided steps.
            float filterSweepCutoffHz = filterSweepStartHz;

            if (filterSweepActive)
            {
                const bool useWholeWindow = (clockMode && (filterSweepScope.load() == FilterSweepScope::wholeWindow))
                                             || (sequencedMode && sequencedSubdivisionActive);

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

            // Tape Stop doesn't fold position at all (it decelerates a
            // plain forward read, then position-exhaustion freezes/holds
            // via its own entirely separate mechanism -- see
            // tapeStopPositionExhausted/positionForRead above) -- always
            // plain forward for it, deliberately excluded from looping so
            // Tape Stop's "grinding to a stop" character stays exactly
            // that, never repeating. Ping-Pong keeps its existing pingPong
            // bounce, which already loops additional round trips for free
            // once elapsed time exceeds one period -- no change needed
            // there. Forward and Filter Down/Up (Step-extension fix) get
            // the loop fold instead of plain forward: once a step's
            // declared duration exceeds one natural unit (one raw slice
            // playthrough -- see the shared Forward/Ping-Pong/Filter
            // Down/Up duration branch above), this is what makes that
            // unit repeat -- wrapping position back to the slice's own
            // start every sliceLength rather than marching on unbounded
            // -- for as long as the render-continue gate keeps letting
            // rendering happen, with no separate loop-boundary
            // bookkeeping needed here. Stretch gets the same loop fold,
            // for the same reason, just with a stretched pass (rather
            // than the raw slice) as the repeating unit -- see the
            // stretchActive branch below for how that pass itself is
            // computed. All three are no-ops whenever a pick's actual
            // duration never reaches a full natural unit/pass in the
            // first place (mathematically identical to plain forward for
            // that shorter span) -- true always in Clock/Slice Length
            // modes (their own duration formulas never let position
            // exceed one natural unit), and in Sequenced mode whenever a
            // step isn't extended past its natural length. Scratch (v1)
            // shares Ping-Pong's own pingPong fold via bounceActive -- its
            // "natural unit" is one Rate cycle rather than one round trip
            // through the slice (see bounceFoldLengthSamples below), but
            // the looping mechanism itself (foldPosition()'s own modulo,
            // once elapsed time exceeds one period) is identical.
            const GranularStretcher::PlaybackStyle grainPlaybackStyle =
                bounceActive ? GranularStretcher::PlaybackStyle::pingPong
              : tapeStopActive ? GranularStretcher::PlaybackStyle::forward
                               : GranularStretcher::PlaybackStyle::loop;

            // Sequenced-mode Ping-Pong half-content window (Step 44):
            // passing HALF the slice's length as the fold boundary (both
            // to GranularStretcher's own grain-start folding below and to
            // the direct-read foldPosition() call further down) makes the
            // round trip reflect within just the first half of the
            // slice's content, rather than the full slice -- a normal-
            // rate there-and-back cycle through half the content take
            // exactly one normal pick's natural length, which is what
            // lets it fit within a single step without needing any rate
            // change at all. Irrelevant (and harmless) for Forward, since
            // foldPosition()'s length argument is only consulted for
            // pingPong/loop -- Stretch (loop) DOES consult it, as the
            // point it wraps back to the slice's own start; unaffected by
            // this half-length special case since that's Ping-Pong-only.
            // Slice Length/Clock modes' own Ping-Pong pace is completely
            // unaffected since this only ever differs from the full slice
            // length when sequencedMode is true.
            const double pingPongFoldLengthSamples = (sequencedMode && pingPongActive)
                ? ((double) currentSliceLength * 0.5)
                : (double) currentSliceLength;

            // Scratch's own fold length (v1, source-domain samples): ONE
            // LEG of its Rate cycle, already clamped to the slice's own
            // content by computeScratchCycleLengthHostSamples() at
            // pick-start -- see currentPickScratchCycleLengthHostSamples's
            // own doc comment. Converted back from that captured host-
            // domain length via playbackRate, the same conversion used to
            // build it in the first place, so the two stay consistent
            // sample for sample.
            const double scratchFoldLengthSamples = scratchActive
                ? (currentPickScratchCycleLengthHostSamples * 0.5 * effectivePickPlaybackRate)
                : 0.0;

            // Shared fold length actually passed to foldPosition()/
            // renderAndAdvance() below, wherever pingPongFoldLengthSamples
            // used to be the only such value -- Ping-Pong's own (slice-
            // content-derived) length, or Scratch's (Rate-cycle-derived)
            // one, per bounceActive's own style check above.
            const double bounceFoldLengthSamples = scratchActive ? scratchFoldLengthSamples : pingPongFoldLengthSamples;

            // Forward/Backward Curve (Scratch v2) -- Linear/Linear for
            // every style but Scratch, which reproduces foldPosition()'s
            // own default (plain constant-speed bounce, byte-identical to
            // before this existed) -- Ping-Pong itself never passes
            // anything else, regardless of bounceActive.
            const EasingCurve scratchForwardCurveForRender = scratchActive ? currentPickScratchForwardCurve : EasingCurve::linear;
            const EasingCurve scratchBackwardCurveForRender = scratchActive ? currentPickScratchBackwardCurve : EasingCurve::linear;

            // Performance mode's Sync-off override (currentPickNativeRateActive)
            // excludes the Time-Stretch granular path the same way stretchActive
            // already forces INTO it regardless of the global Pitch Mode --
            // a per-pick override of which render path runs, not just of the
            // rate value used within one. Native/unsynced playback means
            // neither pitch-shifted nor tempo-matched, which the direct-read
            // path below already gives for free; the granular path's default
            // (non-Stretch) branch derives its own rate from block-level
            // sourceHopSamples/repitchRatio, which a per-pick rate override
            // can't reach by substitution alone. Stretch itself is
            // deliberately exempt (see its own doc comment just below) --
            // its character is independent of Sync, same as it's already
            // independent of Pitch Mode.
            if ((timeStretchMode && ! currentPickNativeRateActive) || stretchActive)
            {
                // Stretch (Step 22) always renders through GranularStretcher,
                // even in Repitch mode -- it's a deliberate character
                // effect, not something that should vanish depending on an
                // unrelated global toggle. It gets its OWN grain parameters
                // here (never the user-facing Pitch Mode Time-Stretch grain
                // size/window shape/pitch shift, none of which apply):
                // outputHopSamples/grainSize come from
                // currentPickStretchGrainSizeMs, and sourceHopSamples is
                // derived from playbackRate/currentPickStretchSpeedMultiplier
                // -- Grain Speed is a FIXED character constant (Step-
                // extension fix), unrelated to how long the pick actually
                // plays. That's governed separately, entirely by the
                // step's own declared length (currentPickLengthInHost-
                // Samples, set at pick-start) -- one full traversal of the
                // slice at Grain Speed's pace is just the "natural unit"
                // for a Stretch pick, the same role a plain slice playback
                // is for Forward; if the declared length is longer than
                // that one pass, grainPlaybackStyle (loop, below) is what
                // makes the SAME pass repeat to fill the remainder, rather
                // than this formula trying to stretch a single pass to
                // fit. Tape Stop's rate multiplier, by contrast,
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
                // Position-exhaustion fix: once tapeStopPositionExhausted,
                // grain spawns stop marching forward entirely (0, rather
                // than the still-decaying-but-nonzero
                // sourceHopSamples * tapeStopRateMultiplier) -- new grains
                // keep spawning at outputHopSamples' normal real-time
                // cadence (that's untouched), but always at the SAME
                // (frozen) source position, holding that content while
                // gain keeps fading, instead of continuing to march the
                // grain engine's own internal position into whatever
                // audio follows the slice in the buffer.
                double grainSourceHopSamples = tapeStopPositionExhausted ? 0.0
                                              : tapeStopActive ? (sourceHopSamples * tapeStopRateMultiplier)
                                              : currentPickBeatQuantized ? (outputHopSamples * srConversionRatio * currentPickQuantizedStretchRatio)
                                                                         : sourceHopSamples;
                double grainGrainSizeHostSamples = grainSizeHostSamples;
                double grainPitchRatio = tapeStopActive ? (pitchRatio * tapeStopRateMultiplier) : pitchRatio;
                GranularStretcher::WindowShape grainWindowShapeToUse = grainWindowShapeForBlock;

                if (stretchActive)
                {
                    grainGrainSizeHostSamples = (double) currentPickStretchGrainSizeMs / 1000.0 * hostSampleRate;
                    grainOutputHopSamples = grainGrainSizeHostSamples * 0.5; // same fixed 50% overlap convention

                    // Grain Speed: a FIXED character constant (Step-
                    // extension fix reverted this back to its original
                    // role) -- how fast, relative to normal playback,
                    // grains march through the source material for ONE
                    // pass. Completely independent of the step's declared
                    // length; that's handled separately, entirely by
                    // grainPlaybackStyle (loop, above) repeating this same
                    // pass for as long as the render-continue gate keeps
                    // rendering.
                    grainSourceHopSamples = grainOutputHopSamples * (effectivePickPlaybackRate / (double) currentPickStretchSpeedMultiplier);

                    grainPitchRatio = 1.0; // no user pitch shift for this style -- fully self-contained/hardcoded
                    grainWindowShapeToUse = GranularStretcher::WindowShape::hardEdge;
                }

                float channelSums[GranularStretcher::maxChannels] = {};
                granularStretcher.renderAndAdvance (sampleBuffer, sourceChannels,
                                                     grainOutputHopSamples, grainSourceHopSamples,
                                                     (double) currentSliceStartSample, bounceFoldLengthSamples, grainPlaybackStyle,
                                                     grainGrainSizeHostSamples, srConversionRatio, grainPitchRatio,
                                                     grainWindowShapeToUse, channelSums,
                                                     scratchForwardCurveForRender, scratchBackwardCurveForRender);

                for (int outCh = 0; outCh < outChannels; ++outCh)
                {
                    const int srcCh = juce::jmin (juce::jmin (outCh, sourceChannels - 1), GranularStretcher::maxChannels - 1);
                    float outputSample = channelSums[srcCh] * (float) gain;

                    if (filterSweepActive)
                        outputSample = filterSweepFilter.processSample (outCh, outputSample);

                    if (bitcrushActive)
                        outputSample = applyBitcrush (outCh, outputSample);

                    if (flangerActive)
                        outputSample = applyFlanger (outCh, outputSample);

                    buffer.addSample (outCh, i, outputSample);
                }
            }
            else
            {
                // Position-exhaustion fix: once tapeStopPositionExhausted, a
                // literal frozen SINGLE sample has no waveform variation --
                // it's inaudible (or a single click, then silence) no
                // matter what the gain envelope is still doing, which
                // defeats the whole point of continuing to render (verified
                // via the debug GAIN-near-zero log: the ramp itself reaches
                // near-zero right at the pick's real end, ~98% of the way
                // through, not at the freeze point -- the frozen single
                // sample was just inaudible for that whole stretch).
                // Instead, loop a short (freezeLoopLengthMs) window of REAL
                // audio ending at the slice's own boundary -- genuinely
                // audible content for the gain envelope to keep fading
                // through. The loop's own playback position is driven by
                // currentPosition's continued (still-decaying)
                // advance past the boundary, not a separate real-time
                // clock, so the loop itself keeps slowing down right along
                // with the rest of the decel, consistent with Tape Stop's
                // character elsewhere. A forward sawtooth loop (jump back
                // to the window start every freezeWindowLength) rather than
                // a crossfaded one -- the small periodic click at each wrap
                // reads as part of the "stuck tape" character rather than a
                // bug, and keeping it simple matches this pass's own scope
                // (mechanism verification, not a polished new effect). A
                // no-op for every other style/case, where this is just
                // currentPosition itself, unchanged.
                double positionForRead = currentPosition;

                if (tapeStopPositionExhausted)
                {
                    constexpr double freezeLoopLengthMs = 25.0;
                    const double freezeLoopLengthSourceSamples =
                        juce::jmax (1.0, (freezeLoopLengthMs / 1000.0) * sampleSampleRate);

                    const double freezeWindowEnd = (double) (schedulingEndSample - 1);
                    const double freezeWindowStart = juce::jmax ((double) currentSliceStartSample,
                                                                   freezeWindowEnd - freezeLoopLengthSourceSamples);
                    const double freezeWindowLength = juce::jmax (1.0, freezeWindowEnd - freezeWindowStart);

                    const double elapsedSinceFreeze = juce::jmax (0.0, currentPosition - freezeWindowEnd);
                    const double loopedOffset = std::fmod (elapsedSinceFreeze, freezeWindowLength);
                    positionForRead = freezeWindowStart + loopedOffset;
                }

                // Shared position-mapping (Step 19): the same foldPosition()
                // GranularStretcher uses for its own grain-start scheduling,
                // so Ping-Pong behaves identically regardless of pitch mode.
                // For Forward this is the identity — foldedReadPosition ==
                // positionForRead exactly, same as before this existed.
                const double foldedReadPosition = (double) currentSliceStartSample
                    + GranularStretcher::foldPosition (positionForRead - (double) currentSliceStartSample,
                                                        bounceFoldLengthSamples, grainPlaybackStyle,
                                                        scratchForwardCurveForRender, scratchBackwardCurveForRender);

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

                    if (bitcrushActive)
                        sample = applyBitcrush (outCh, sample);

                    if (flangerActive)
                        sample = applyFlanger (outCh, sample);

                    buffer.addSample (outCh, i, sample);
                }
            }

            // Flanger delay line write position: advanced once per OUTPUT
            // sample here (not per channel above), same lockstep reasoning
            // as the read index computed alongside effectiveFlangerDelay-
            // Samples earlier -- both channels' applyFlanger() calls above
            // already wrote this sample's dry content at the PRE-advance
            // index, so this just moves the line forward one slot for the
            // next sample to read/write against.
            if (flangerActive && flangerDelayBuffer.getNumSamples() > 0)
                flangerDelayWriteIndex = (flangerDelayWriteIndex + 1) % flangerDelayBuffer.getNumSamples();

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
            // Sequenced-mode Ping-Pong (Step 44) needs no rate change at
            // all -- position advances at exactly the same rate as every
            // other style/mode; see the halved fold-length above/below
            // instead, which is what actually compresses the round trip
            // into a normal pick's timeframe.
            const double effectivePlaybackRate = tapeStopActive ? (effectivePickPlaybackRate * tapeStopRateMultiplier)
                                                : currentPickBeatQuantized ? ((sampleSampleRate / hostSampleRate) * currentPickQuantizedStretchRatio)
                                                                           : effectivePickPlaybackRate;
            currentPosition += effectivePlaybackRate;
            samplesSincePickStart += 1.0;

            // Control mode Gate release: counts real time since the release
            // note-off arrived (controlGateReleaseGain above already used
            // this sample's PRE-increment value, same convention
            // samplesSincePickStart itself follows). Once the ramp reaches
            // silence, force the pick to actually stop -- controlMode's own
            // per-sample dispatch won't run again until the NEXT sample, so
            // this is the one place that can cut it off right on schedule.
            if (controlGateReleaseActive)
            {
                controlGateReleaseElapsedSamples += 1.0;

                if (controlGateReleaseElapsedSamples >= fadeOutSamplesRequested)
                {
                    hasCurrentPick = false;
                    controlGateReleaseActive = false;
                    controlCurrentlySoundingNote = -1;
                }
            }
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
        // Sequenced mode (Step 47) also needs this incrementing -- it's
        // reused there as the Subdivide retrigger scheduler's own clock
        // (see the sequencedMode branch above), not just the Filter Sweep
        // Whole Window progress fraction.
        if (clockMode || sequencedMode)
            samplesSinceWindowStart += 1.0;
    }
}

void SlicerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    renderPickBlock (buffer, midiMessages);
    processSendBuses (buffer);
}

void SlicerAudioProcessor::processSendBuses (juce::AudioBuffer<float>& buffer)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    // Send Amount: Sequenced mode (Pass 2) uses the currently-rendering
    // pick's own per-step Send Amount override (captured at that step's
    // own trigger -- see currentPickDelaySendAmount/currentPickReverbSendAmount's
    // own doc comment in PluginProcessor.h; 0.0f/no send if that step never
    // had one set, per this feature's own opt-in spec). Every other trigger
    // mode has no per-note or global Send Amount control at all, so it
    // keeps Pass 1's original fixed 50% unchanged. `buffer` already holds
    // this block's full dry single-voice output (or silence) at this
    // point.
    const bool sequencedModeNow = (triggerMode.load() == TriggerMode::sequenced);
    const float delaySendAmount = sequencedModeNow ? currentPickDelaySendAmount : 0.5f;
    const float reverbSendAmount = sequencedModeNow ? currentPickReverbSendAmount : 0.5f;

    delayBusBuffer.setSize (numChannels, numSamples, false, false, true);
    reverbBusBuffer.setSize (numChannels, numSamples, false, false, true);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        delayBusBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
        delayBusBuffer.applyGain (ch, 0, numSamples, delaySendAmount);

        reverbBusBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
        reverbBusBuffer.applyGain (ch, 0, numSamples, reverbSendAmount);
    }

    // --- Character ramp (Pass 2): ease each of the four live bus-character
    // values (Time/Feedback/Size/Decay) toward its own ramp TARGET every
    // block, rather than snapping to it instantly -- see
    // delayBusTimeMsRampTargetValue's own doc comment in PluginProcessor.h
    // for who writes each target. A step's own character override only
    // ever moves the target (processBlock()'s sequencedMode branch); this
    // is what actually moves the LIVE value the DSP just below reads, so
    // an override landing mid-decay/mid-tail glides into place instead of
    // jumping (this is a continuously-running processor, not a per-note
    // instance -- an instant jump would click). One-pole exponential
    // approach, a fixed ~40ms time constant regardless of block size --
    // short enough to read as "live" as steps fire, long enough that even
    // a large jump (e.g. a 2000ms Delay Time swing) never reads as a
    // click. A manual dial turn (setDelayBusTimeMs() etc.) re-arms both
    // the live value and its target to the same number, so this is a
    // no-op immediately afterward rather than fighting the user's own
    // adjustment. ---
    constexpr float busCharacterRampSeconds = 0.04f;
    const double sampleRateForRamp = getSampleRate();
    const float busCharacterRampCoeff = sampleRateForRamp > 0.0
        ? 1.0f - std::exp ((float) (-(double) numSamples / (busCharacterRampSeconds * sampleRateForRamp)))
        : 1.0f;

    auto rampBusCharacterToward = [busCharacterRampCoeff] (std::atomic<float>& live, const std::atomic<float>& target)
    {
        const float current = live.load();
        const float dest = target.load();

        if (current != dest)
            live.store (current + (dest - current) * busCharacterRampCoeff);
    };

    rampBusCharacterToward (delayBusTimeMsValue, delayBusTimeMsRampTargetValue);
    rampBusCharacterToward (delayBusFeedbackValue, delayBusFeedbackRampTargetValue);
    rampBusCharacterToward (reverbBusSizeValue, reverbBusSizeRampTargetValue);
    rampBusCharacterToward (reverbBusDecayValue, reverbBusDecayRampTargetValue);

    // --- Delay bus: a hand-rolled feedback loop around juce::dsp::DelayLine
    // (the DelayLine class itself has no built-in feedback, so this pushes
    // input-plus-fed-back-delayed-sample and pops the delayed sample back
    // out, same per-sample push/pop shape the Flanger style's own
    // hand-rolled circular buffer already uses, just via JUCE's
    // interpolated line instead). Persists across blocks -- and across
    // picks -- via the DelayLine's own internal state, so a tail keeps
    // ringing after the triggering pick stops. ---
    const float delaySamples = juce::jlimit (1.0f, (float) delayBusLine.getMaximumDelayInSamples(),
                                              (float) (delayBusTimeMsValue.load() * 0.001 * getSampleRate()));
    delayBusLine.setDelay (delaySamples);
    const float delayFeedback = delayBusFeedbackValue.load();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data = delayBusBuffer.getWritePointer (ch);

        for (int i = 0; i < numSamples; ++i)
        {
            const float delayed = delayBusLine.popSample (ch);
            delayBusLine.pushSample (ch, data[i] + delayed * delayFeedback);
            data[i] = delayed;
        }
    }

    // --- Reverb bus: juce::dsp::Reverb, forced 100% wet -- this bus's own
    // Return Level (below) does the mixing back into the main output, not
    // the Reverb DSP's own internal dry/wet blend, so every bus here uses
    // the same send/return model. Decay maps onto damping, inverted (a
    // higher Decay value means less high-frequency damping, i.e. a
    // longer-sounding tail); Size maps directly onto roomSize. ---
    juce::Reverb::Parameters reverbParams;
    reverbParams.roomSize = reverbBusSizeValue.load();
    reverbParams.damping = 1.0f - reverbBusDecayValue.load();
    reverbParams.wetLevel = 1.0f;
    reverbParams.dryLevel = 0.0f;
    reverbParams.width = 1.0f;
    reverbBusDSP.setParameters (reverbParams);

    juce::dsp::AudioBlock<float> reverbBlock (reverbBusBuffer);
    juce::dsp::ProcessContextReplacing<float> reverbContext (reverbBlock);
    reverbBusDSP.process (reverbContext);

    // --- Mix each bus's wet output back into the main output, scaled by
    // its own Return Level. ---
    const float delayReturn = delayBusReturnLevelValue.load();
    const float reverbReturn = reverbBusReturnLevelValue.load();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        buffer.addFrom (ch, 0, delayBusBuffer, ch, 0, numSamples, delayReturn);
        buffer.addFrom (ch, 0, reverbBusBuffer, ch, 0, numSamples, reverbReturn);
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
        tempoTrimStartSample.store (0);
        tempoTrimEndSample.store (sampleBuffer.getNumSamples());

        manualPoints.clear(); // positions from the old sample don't mean anything here
        excludedPoints.clear();
        hasCurrentPick = false; // don't let a stale pick read past the new buffer's end
        clockModeInitialized = false;
        clockCurrentPickValid = false;
        auditionActive.store (false); // Step 25 -- a stale audition loop from the old buffer/trim makes no sense against the new one
        auditionPlaybackPositionForUI.store (-1); // Step 28 -- and neither does its stale playhead indicator
    }

    undoManager.clearUndoHistory(); // old undo steps reference positions in a different file now

    redetectSlices (defaultSensitivity, computeMinimumHoldoffMs());
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

    const int trimStart = tempoTrimStartSample.load();
    const int trimEnd = tempoTrimEndSample.load();
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
    auto autoSlices = transientDetector.detectSlices (sensitivity, computeMinimumHoldoffMs(), trimStart, trimEnd);

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

int SlicerAudioProcessor::findNearestGridSample (int rawSample) const
{
    const double gridBeats = getNoteValueBeats (performanceTrimGridIndex.load());

    if (gridBeats <= 0.0)
        return rawSample;

    const double originalBpm = getCalculatedOriginalBpm();

    if (originalBpm <= 0.0 || sampleSampleRate <= 0.0)
        return rawSample;

    // Same beats<->samples round-trip quantizeOnsetToGrid() above uses,
    // anchored at tempoTrimStartSample instead of a passed-in trim start --
    // see this function's own comment for why.
    const int anchor = tempoTrimStartSample.load();
    const double offsetSeconds = (double) (rawSample - anchor) / sampleSampleRate;
    const double offsetBeats = offsetSeconds * (originalBpm / 60.0);
    const double nearestGridStep = std::round (offsetBeats / gridBeats);
    const double quantizedBeats = nearestGridStep * gridBeats;
    const double quantizedSeconds = quantizedBeats * (60.0 / originalBpm);
    const double quantizedSampleDouble = (double) anchor + quantizedSeconds * sampleSampleRate;

    return (int) std::llround (quantizedSampleDouble);
}

void SlicerAudioProcessor::resetSequencerGrid()
{
    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();
    sequencerGrid.assign ((size_t) juce::jmax (0, rows * columns), -1);
    sequencerCellParameterOverrides.clear(); // Step 45 -- dimensions just changed, old flat indices are meaningless now
    sequencerCellExtendedLengthSteps.clear(); // Step-extension (Pass 1) -- same reasoning
}

//==============================================================================
// MIDI input dispatch layer + Sequencer pattern bank (Pass 1: immediate
// recall; Pass 2: Set Interval/End of Pattern switch timing)
//==============================================================================

void SlicerAudioProcessor::handleIncomingMidi (const juce::MidiBuffer& midiMessages, bool hostTransportPlaying)
{
    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();

        if (message.isNoteOn())
            dispatchNoteOn (message.getNoteNumber(), message.getFloatVelocity(), hostTransportPlaying);
        else if (message.isNoteOff())
            dispatchNoteOff (message.getNoteNumber());
    }
}

void SlicerAudioProcessor::dispatchNoteOn (int noteNumber, float velocity01, bool hostTransportPlaying)
{
    // The routing point: checks current context (TriggerMode) and calls
    // whichever handler applies. Sequenced mode's pattern bank, Performance
    // mode's state bank (Pass 1), and Control mode's slice/keyswitch
    // dispatch are the handlers today.
    switch (triggerMode.load())
    {
        case TriggerMode::sequenced:
            if (midiLearnArmed.load())
                completeMidiLearn (noteNumber);
            else
                handlePatternSwitchNoteOn (noteNumber);
            break;

        case TriggerMode::performance:
            handlePerformanceStateNoteOn (noteNumber, hostTransportPlaying);
            break;

        case TriggerMode::control:
            handleControlNoteOn (noteNumber, velocity01);
            break;

        case TriggerMode::sliceLength:
        case TriggerMode::clock:
        default:
            break; // MIDI ignored entirely here
    }
}

void SlicerAudioProcessor::dispatchNoteOff (int noteNumber)
{
    if (triggerMode.load() == TriggerMode::control)
        handleControlNoteOff (noteNumber);
}

SlicerAudioProcessor::SequencerPatternSnapshot SlicerAudioProcessor::captureCurrentSequencerPatternSnapshot() const
{
    SequencerPatternSnapshot snapshot;
    snapshot.populated = true;
    snapshot.rows = getSequencerNumRows();
    snapshot.columns = getSequencerNumSteps();
    snapshot.stepResolutionIndex = stepResolutionIndex.load();
    snapshot.patternLengthBarsIndex = patternLengthBarsIndex.load();
    snapshot.grid = sequencerGrid;
    snapshot.parameterOverrides = sequencerCellParameterOverrides;
    snapshot.extendedLengthSteps = sequencerCellExtendedLengthSteps;
    return snapshot;
}

void SlicerAudioProcessor::handlePatternSwitchNoteOn (int noteNumber)
{
    if (! patternBank[(size_t) noteNumber].populated)
        return; // empty slot -- no-op regardless of timing mode, same as Pass 1

    if (patternSwitchTiming.load() == PatternSwitchTiming::immediate)
    {
        handleSequencerPatternRecallNoteOn (noteNumber);
        return;
    }

    // Set Interval / End of Pattern: don't switch yet -- arm the pending
    // target and let the per-sample boundary check in processBlock()'s
    // sequencedMode branch apply it once the chosen boundary is crossed. A
    // newer note-on arriving before that boundary just overwrites this same
    // atomic, so the newest one always wins -- there's never more than one
    // pending switch to track.
    pendingPatternSwitchNote.store (noteNumber);
    patternSwitchIntervalBoundaryArmed = false; // force Set Interval's "next occurrence" to be recomputed fresh from wherever ppq is on the very next per-sample check
}

void SlicerAudioProcessor::handleSequencerPatternRecallNoteOn (int noteNumber)
{
    const auto& snapshot = patternBank[(size_t) noteNumber];

    if (! snapshot.populated)
        return; // empty slot -- no-op, current pattern keeps playing undisturbed

    sequencerGrid = snapshot.grid;
    sequencerCellParameterOverrides = snapshot.parameterOverrides;
    sequencerCellExtendedLengthSteps = snapshot.extendedLengthSteps;
    stepResolutionIndex.store (snapshot.stepResolutionIndex);
    patternLengthBarsIndex.store (snapshot.patternLengthBarsIndex);
    activePatternBankSlot.store (noteNumber);

    // Force the step-boundary tracker to re-sync against the newly recalled
    // grid on the very next check, below in processBlock() -- same re-init
    // setTriggerMode() already relies on when switching INTO Sequenced mode.
    sequencedModeInitialized = false;
}

void SlicerAudioProcessor::completeMidiLearn (int noteNumber)
{
    patternBank[(size_t) noteNumber] = pendingSaveSnapshot;
    midiLearnArmed.store (false);
}

void SlicerAudioProcessor::armMidiLearnForPatternSave()
{
    const juce::ScopedLock sl (sampleLock);
    pendingSaveSnapshot = captureCurrentSequencerPatternSnapshot();
    midiLearnArmed.store (true);
}

void SlicerAudioProcessor::cancelMidiLearn()
{
    midiLearnArmed.store (false);
}

std::array<bool, 128> SlicerAudioProcessor::getPopulatedPatternBankSlots() const
{
    const juce::ScopedLock sl (sampleLock);
    std::array<bool, 128> populated {};

    for (size_t i = 0; i < patternBank.size(); ++i)
        populated[i] = patternBank[i].populated;

    return populated;
}

//==============================================================================
// Performance mode state bank (click-to-focus + auto-save)
//==============================================================================

void SlicerAudioProcessor::handlePerformanceStateNoteOn (int noteNumber, bool hostTransportPlaying)
{
    // Physical MIDI in Performance mode is playback-only -- the on-screen
    // keyboard's click handler (setFocusedPerformanceStateSlot(), below) is
    // the only thing that ever changes focus. Pressing the FOCUSED slot's
    // own key live-auditions whatever's currently being edited
    // (performanceWorkingState, played via the shared trim atomics --
    // exactly the same live objects the parameter panel/waveform trim
    // handles write into); pressing any OTHER slot's key plays back
    // whatever's already saved there, via a frozen copy of its own
    // independent snapshot (including its own saved trim), so auditioning
    // it can never disturb performanceWorkingState or the focused slot's
    // in-progress edits. An empty, unfocused slot is a no-op regardless of
    // Quantize Recall -- checked up front, before either the immediate or
    // deferred path below runs, same as Pass 1.
    if (noteNumber != focusedPerformanceStateSlot.load() && ! performanceStateBank[(size_t) noteNumber].populated)
        return; // empty, unfocused slot -- no-op, whatever's already playing keeps playing undisturbed

    // Quantize Recall (see its own doc comment above): defer to the next
    // grid point instead of switching right now, UNLESS the host transport
    // isn't playing -- there's no meaningful beat position to quantize
    // against without it, so this falls back to the immediate path below,
    // same as Quantize Recall being off entirely.
    if (performanceQuantizeRecallEnabled.load() && hostTransportPlaying)
    {
        // A newer note-on before the boundary just overwrites this same
        // atomic, so the newest one always wins -- there's never more than
        // one pending recall to track. Re-armed (false) so the per-sample
        // boundary check in processBlock() recomputes "next occurrence"
        // fresh from wherever ppq is right now, not from whenever an
        // earlier pending note-on arrived.
        pendingPerformanceRecallNote.store (noteNumber);
        performanceQuantizeRecallBoundaryArmed = false;
        return;
    }

    applyPerformanceStateRecall (noteNumber);
}

void SlicerAudioProcessor::applyPerformanceStateRecall (int noteNumber)
{
    if (noteNumber == focusedPerformanceStateSlot.load())
    {
        performancePlaybackIsFocused = true;
    }
    else
    {
        const auto& snapshot = performanceStateBank[(size_t) noteNumber];

        if (! snapshot.populated)
            return; // slot emptied (or lost focus) between the note-on that armed a deferred recall and this boundary -- no-op, same as the immediate path's own check

        currentlyPlayingPerformanceSnapshot = snapshot;
        performancePlaybackIsFocused = false;
    }

    // Force the very next per-sample check in processBlock()'s
    // performanceMode branch to start a fresh pick from whichever source
    // was just selected above -- same same-call, same-lock handoff
    // handleSequencerPatternRecallNoteOn() already uses via
    // sequencedModeInitialized.
    performanceRecallPending = true;
}

void SlicerAudioProcessor::setFocusedPerformanceStateSlot (int noteNumber)
{
    if (noteNumber < 0 || noteNumber >= 128)
        return;

#if JUCE_DEBUG
    // TEMPORARY DEBUG (Performance mode freeze investigation) -- this
    // function runs on the message thread, so I/O here is safe (unlike
    // inside processBlock()); marks the window during which this thread
    // might be blocked waiting for sampleLock, for freezeWatchdog to report.
    debugFocusChangeNoteNumber.store (noteNumber, std::memory_order_relaxed);
    debugFocusChangeStartMs.store ((juce::int64) juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
    debugFocusChangeInProgress.store (true, std::memory_order_relaxed);
    std::cerr << "[UI] setFocusedPerformanceStateSlot(" << noteNumber << ") -- about to acquire sampleLock" << std::endl;
#endif

    const juce::ScopedLock sl (sampleLock);

#if JUCE_DEBUG
    std::cerr << "[UI] setFocusedPerformanceStateSlot(" << noteNumber << ") -- sampleLock acquired" << std::endl;
#endif

    const int previous = focusedPerformanceStateSlot.load();

    if (noteNumber == previous)
    {
#if JUCE_DEBUG
        std::cerr << "[UI] setFocusedPerformanceStateSlot(" << noteNumber << ") -- already focused, no-op" << std::endl;
        debugFocusChangeInProgress.store (false, std::memory_order_relaxed);
#endif
        return; // already focused -- nothing to save away from or load
    }

    // Auto-save (replaces the old MIDI-Learn "Save to..." button entirely):
    // whatever was being edited in the previously-focused slot is captured
    // now, the instant focus moves away from it.
    if (previous >= 0)
    {
        PerformanceStateSnapshot saved = performanceWorkingState;
        saved.populated = true;
        saved.trimStartSample = trimStartSample.load();
        saved.trimEndSample = trimEndSample.load();
        performanceStateBank[(size_t) previous] = saved;
    }

    // Load the new slot: its existing saved state, or a fresh default to
    // start editing from if it has none yet (matching the Artillery 2
    // reference) -- the same seed values the constructor gives
    // performanceWorkingState initially. Deliberately does NOT write this
    // default into performanceStateBank[noteNumber] -- it only becomes a
    // real saved slot (and lights up the keyboard's populated highlight)
    // once focus actually moves away from it, same as every other slot.
    const auto& existing = performanceStateBank[(size_t) noteNumber];

    if (existing.populated)
    {
        performanceWorkingState = existing;
        trimStartSample.store (existing.trimStartSample);
        trimEndSample.store (existing.trimEndSample);
    }
    else
    {
        performanceWorkingState = PerformanceStateSnapshot {};
        performanceWorkingState.populated = true; // lets this slot's own key live-audition immediately, even before its first auto-save

        for (int i = 0; i < numSequencerCellParameters; ++i)
            performanceWorkingState.parameterValues[(size_t) i] = getSequencerCellParameterGlobalValue (i);

        trimStartSample.store (0);
        trimEndSample.store (sampleBuffer.getNumSamples());
    }

    focusedPerformanceStateSlot.store (noteNumber);

#if JUCE_DEBUG
    std::cerr << "[UI] setFocusedPerformanceStateSlot(" << noteNumber << ") -- done" << std::endl;
    debugFocusChangeInProgress.store (false, std::memory_order_relaxed);
#endif
}

std::array<bool, 128> SlicerAudioProcessor::getPopulatedPerformanceStateBankSlots() const
{
    const juce::ScopedLock sl (sampleLock);
    std::array<bool, 128> populated {};

    for (size_t i = 0; i < performanceStateBank.size(); ++i)
        populated[i] = performanceStateBank[i].populated;

    return populated;
}

int SlicerAudioProcessor::getPerformanceWorkingStyle() const
{
    const juce::ScopedLock sl (sampleLock);
    return performanceWorkingState.style;
}

void SlicerAudioProcessor::setPerformanceWorkingStyle (int style)
{
    const juce::ScopedLock sl (sampleLock);
    performanceWorkingState.style = style;
}

float SlicerAudioProcessor::getPerformanceWorkingParameterValue (int index) const
{
    const juce::ScopedLock sl (sampleLock);

    if (index < 0 || index >= numSequencerCellParameters)
        return 0.0f;

    return performanceWorkingState.parameterValues[(size_t) index];
}

void SlicerAudioProcessor::setPerformanceWorkingParameterValue (int index, float value)
{
    const juce::ScopedLock sl (sampleLock);

    if (index < 0 || index >= numSequencerCellParameters)
        return;

    performanceWorkingState.parameterValues[(size_t) index] = value;
}

bool SlicerAudioProcessor::getPerformanceWorkingLoop() const
{
    const juce::ScopedLock sl (sampleLock);
    return performanceWorkingState.loop;
}

void SlicerAudioProcessor::setPerformanceWorkingLoop (bool loop)
{
    const juce::ScopedLock sl (sampleLock);
    performanceWorkingState.loop = loop;
}

bool SlicerAudioProcessor::getPerformanceWorkingSync() const
{
    const juce::ScopedLock sl (sampleLock);
    return performanceWorkingState.sync;
}

void SlicerAudioProcessor::setPerformanceWorkingSync (bool sync)
{
    const juce::ScopedLock sl (sampleLock);
    performanceWorkingState.sync = sync;
}

//==============================================================================
// Control mode -- piano-roll slice triggering with keyswitch style selection
//==============================================================================

void SlicerAudioProcessor::handleControlNoteOn (int noteNumber, float velocity01)
{
    const int base = controlBaseNote.load();

    // Fixed keyswitch mapping (getControlKeyswitchNote()'s own inverse):
    // noteNumber is style playbackStyleIndex's keyswitch exactly when
    // noteNumber == base - 1 - playbackStyleIndex, i.e.
    // playbackStyleIndex == base - 1 - noteNumber. Checked first -- cheap
    // arithmetic, and the two ranges never overlap by construction anyway
    // (keyswitches always sit below base, slices always at or above it).
    const int keyswitchStyle = base - 1 - noteNumber;

    if (keyswitchStyle >= 0 && keyswitchStyle < numPlaybackStyleOptions)
    {
        controlActiveStyle.store (keyswitchStyle);
        return; // keyswitches never make sound
    }
    const int sliceIndex = noteNumber - base;
    const int numAvailableSlices = getSequencerNumRows(); // same 32-slice cap as the Sequencer's own rows

    if (sliceIndex < 0 || sliceIndex >= numAvailableSlices)
        return; // outside both the keyswitch and slice ranges -- no-op

    controlNoteOnPending = true;
    controlPendingSliceIndex = sliceIndex;
    controlPendingStyle = controlActiveStyle.load();
    controlPendingVelocityGain = (double) juce::jlimit (0.0f, 1.0f, velocity01);
    controlPendingNoteNumber = noteNumber;
}

void SlicerAudioProcessor::handleControlNoteOff (int noteNumber)
{
    if (! controlGateModeActive.load())
        return; // Trigger mode ignores note-off entirely

    if (noteNumber != controlCurrentlySoundingNote || ! hasCurrentPick)
        return; // not the note that's actually still sounding -- nothing to release

    controlGateReleaseActive = true;
    controlGateReleaseElapsedSamples = 0.0;
}

int SlicerAudioProcessor::getSequencerCellStyle (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return -1;

    const size_t idx = (size_t) (row * columns + column);
    return idx < sequencerGrid.size() ? sequencerGrid[idx] : -1;
}

void SlicerAudioProcessor::setSequencerCell (int row, int column, int style)
{
    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return;

    if ((int) sequencerGrid.size() != rows * columns)
        resetSequencerGrid(); // defensive -- dimensions drifted out from under us somehow

    if (style >= 0)
    {
        // Structural monophony (Step 37, v1): clear any other active cell
        // in this SAME COLUMN across every row first, so "only one voice"
        // is true at the INPUT level the instant a pattern is drawn, not
        // just something the playback engine happens to enforce
        // afterward -- and it's what avoids needing a tie-break rule
        // entirely at playback time. Their parameter overrides go with
        // them (Step 45) -- a cell that's no longer active has no
        // meaningful style-specific parameters left to override.
        for (int r = 0; r < rows; ++r)
        {
            sequencerGrid[(size_t) (r * columns + column)] = -1;
            sequencerCellParameterOverrides.erase (r * columns + column);
            sequencerCellExtendedLengthSteps.erase (r * columns + column); // Step-extension (Pass 1) -- a cleared cell's own extension goes with it too
        }
    }

    // This cell's own style is changing (or clearing) too (Step 45) --
    // always drop its overrides first, so re-painting the same physical
    // cell later never resurrects a stale override left over from a
    // completely different style.
    sequencerCellParameterOverrides.erase (row * columns + column);
    sequencerCellExtendedLengthSteps.erase (row * columns + column); // Step-extension (Pass 1) -- same reasoning
    sequencerGrid[(size_t) (row * columns + column)] = style;
}

int SlicerAudioProcessor::getSequencerCellExtendedLengthSteps (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return 0;

    const auto it = sequencerCellExtendedLengthSteps.find (row * columns + column);
    return it != sequencerCellExtendedLengthSteps.end() ? it->second : 0;
}

void SlicerAudioProcessor::setSequencerCellExtendedLengthSteps (int row, int column, int lengthSteps)
{
    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return;

    if ((int) sequencerGrid.size() != rows * columns)
        resetSequencerGrid(); // defensive -- dimensions drifted out from under us somehow, mirrors setSequencerCell()

    const int idx = row * columns + column;

    if (sequencerGrid[(size_t) idx] < 0)
        return; // only an already-active cell can be extended

    const int clampedLength = juce::jlimit (1, columns - column, lengthSteps);

    // Growing into columns another row already occupies clears those
    // conflicting cells -- the exact same per-column monophony rule
    // setSequencerCell() enforces above for a plain single-cell draw, just
    // applied across the whole newly-claimed span instead of one column.
    // This row's OWN later cells (if any) are deliberately left alone --
    // only ANOTHER row's occupancy counts as a conflict here.
    for (int offset = 1; offset < clampedLength; ++offset)
    {
        const int col = column + offset;

        for (int r = 0; r < rows; ++r)
        {
            if (r == row)
                continue;

            const int otherIdx = r * columns + col;

            if (sequencerGrid[(size_t) otherIdx] >= 0)
            {
                sequencerGrid[(size_t) otherIdx] = -1;
                sequencerCellParameterOverrides.erase (otherIdx);
                sequencerCellExtendedLengthSteps.erase (otherIdx);
            }
        }
    }

    sequencerCellExtendedLengthSteps[idx] = clampedLength;
}

int SlicerAudioProcessor::getSequencerNaturalLengthSteps (int row) const
{
    const juce::ScopedLock sl (sampleLock);

    if (row < 0 || row >= (int) slices.size())
        return 1;

    const auto& slice = slices[(size_t) row];
    const int sliceLength = slice.endSample - slice.startSample;
    const double originalBpm = getCalculatedOriginalBpm();

    int naturalSteps = 1;

    if (sliceLength > 0 && sampleSampleRate > 0.0 && originalBpm > 0.0)
    {
        const double sliceSeconds = (double) sliceLength / sampleSampleRate;
        const double naturalBeats = sliceSeconds * (originalBpm / 60.0);
        const double stepBeats = getNoteValueBeats (stepResolutionIndex.load());

        if (stepBeats > 0.0)
            naturalSteps = juce::jmax (1, juce::roundToInt (naturalBeats / stepBeats));
    }

    return naturalSteps;
}

int SlicerAudioProcessor::getSequencerCellDeclaredLengthSteps (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return 1;

    const int naturalSteps = getSequencerNaturalLengthSteps (row);
    const int extendedOverride = getSequencerCellExtendedLengthSteps (row, column);

    return juce::jmax (naturalSteps, extendedOverride);
}

#if JUCE_DEBUG
void SlicerAudioProcessor::drainDebugTapeStopEvents()
{
    // TEMPORARY DEBUG -- remove once step-extension Tape Stop testing is
    // done. UI-thread only: does the actual DBG()/console I/O the audio
    // thread itself never does (see the mailbox members' own doc comment
    // in PluginProcessor.h). No lock taken here at all -- every field
    // below is its own atomic, and nothing here touches sampleLock or any
    // audio-thread-only state.
    if (debugTapeStopPickStartEventPending.exchange (false))
    {
        DBG ("TapeStop pick-start: row=" << debugTapeStopPickStartRow.load()
             << " step=" << debugTapeStopPickStartStep.load()
             << " declaredLengthSteps=" << debugTapeStopPickStartDeclaredLengthSteps.load()
             << " declaredLengthHostSamples=" << debugTapeStopPickStartDeclaredLengthHostSamples.load()
             << " samplesUntilNextActiveStep=" << debugTapeStopPickStartSamplesUntilNextActiveStep.load()
             << " => durationHostSamples=" << debugTapeStopPickStartDurationHostSamples.load());
    }

    if (debugTapeStopFreezeEventPending.exchange (false))
    {
        DBG ("TapeStop FREEZE activated: samplesSincePickStart=" << debugTapeStopFreezeSamplesSincePickStart.load()
             << " durationHostSamples=" << debugTapeStopFreezeDurationHostSamples.load()
             << " currentPosition=" << debugTapeStopFreezePosition.load()
             << " schedulingEndSample=" << debugTapeStopFreezeSchedulingEndSample.load());
    }

    if (debugTapeStopStopEventPending.exchange (false))
    {
        DBG ("TapeStop render STOPPED: samplesSincePickStart=" << debugTapeStopStopSamplesSincePickStart.load()
             << " durationHostSamples=" << debugTapeStopStopDurationHostSamples.load()
             << " everFroze=" << (debugTapeStopStopEverFroze.load() ? "yes" : "no"));
    }

    if (debugTapeStopGainNearZeroEventPending.exchange (false))
    {
        DBG ("TapeStop GAIN near zero: samplesSincePickStart=" << debugTapeStopGainNearZeroSamplesSincePickStart.load()
             << " durationHostSamples=" << debugTapeStopGainNearZeroDurationHostSamples.load()
             << " gain=" << debugTapeStopGainNearZeroGainValue.load()
             << " rateMultiplier=" << debugTapeStopGainNearZeroRateMultiplier.load()
             << " wasExhausted=" << (debugTapeStopGainNearZeroWasExhausted.load() ? "yes" : "no")
             << " wasGranular=" << (debugTapeStopGainNearZeroWasGranular.load() ? "yes" : "no"));
    }
}

void SlicerAudioProcessor::drainDebugStretchEvents()
{
    // TEMPORARY DEBUG -- remove once Stretch/Forward/Ping-Pong/Filter
    // Down/Up step-extension investigation is done. Same UI-thread-only,
    // no-lock pattern as drainDebugTapeStopEvents() above.
    if (debugStretchPickStartEventPending.exchange (false))
    {
        DBG ("Stretch pick-start: row=" << debugStretchPickStartRow.load()
             << " step=" << debugStretchPickStartStep.load()
             << " declaredLengthSteps=" << debugStretchPickStartDeclaredLengthSteps.load()
             << " declaredLengthHostSamples=" << debugStretchPickStartDeclaredLengthHostSamples.load()
             << " naturalLengthHostSamples=" << debugStretchPickStartNaturalLengthHostSamples.load()
             << " speedMultiplier=" << debugStretchPickStartSpeedMultiplier.load()
             << " passLengthHostSamples=" << debugStretchPickStartPassLengthHostSamples.load()
             << " samplesUntilNextActiveStep=" << debugStretchPickStartSamplesUntilNextActiveStep.load()
             << " => finalLengthHostSamples=" << debugStretchPickStartFinalLengthHostSamples.load());
    }

    if (debugLoopStylePickStartEventPending.exchange (false))
    {
        DBG (getPlaybackStyleName (debugLoopStylePickStartStyleIndex.load()) << " pick-start: row=" << debugLoopStylePickStartRow.load()
             << " step=" << debugLoopStylePickStartStep.load()
             << " declaredLengthSteps=" << debugLoopStylePickStartDeclaredLengthSteps.load()
             << " declaredLengthHostSamples=" << debugLoopStylePickStartDeclaredLengthHostSamples.load()
             << " naturalLengthHostSamples=" << debugLoopStylePickStartNaturalLengthHostSamples.load()
             << " samplesUntilNextActiveStep=" << debugLoopStylePickStartSamplesUntilNextActiveStep.load()
             << " => finalLengthHostSamples=" << debugLoopStylePickStartFinalLengthHostSamples.load());
    }
}
#endif

void SlicerAudioProcessor::clearSequence()
{
    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if ((int) sequencerGrid.size() != rows * columns)
        resetSequencerGrid();

    std::fill (sequencerGrid.begin(), sequencerGrid.end(), -1);
    sequencerCellParameterOverrides.clear(); // Step 45
    sequencerCellExtendedLengthSteps.clear(); // Step-extension (Pass 1)
}

juce::String SlicerAudioProcessor::getSequencerCellParameterName (int index)
{
    if (index < 0 || index >= numSequencerCellParameters)
        return {};

    return sequencerCellParameterNames[(size_t) index];
}

bool SlicerAudioProcessor::isSequencerCellParameterDiscrete (int index)
{
    return index == 1 || index == 2 || index == 5 || index == 7 || index == 9 || index == 10 || index == 11 || index == 12 || index == 14 || index == 16 || index == 18 || index == 20; // Filter Type, Curve Shape (Step 46); Subdivide (Step 47); Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Rate, Forward Curve, Backward Curve (Scratch v1/v2); Delay Time Mode, Mix Mode, Feedback Mode (Flanger); Volume Mode
}

bool SlicerAudioProcessor::isSequencerCellParameterSteppedSlider (int index)
{
    return index == 5; // Subdivide (Step 47) -- see its declaration in PluginProcessor.h for why
}

bool SlicerAudioProcessor::isSequencerCellParameterSwept (int index)
{
    return index == 6 || index == 8 || index == 13 || index == 15 || index == 17 || index == 19; // Sample Rate Reduction, Bit Depth (Step 49); Delay Time, Mix, Feedback (Flanger); Volume -- see declaration in PluginProcessor.h
}

int SlicerAudioProcessor::getSequencerCellParameterNumOptions (int index)
{
    if (index == 1) return numFilterSweepFilterTypeOptions;
    if (index == 2) return numCurveShapeOptions;
    if (index == 5) return numNoteValueOptions + 1; // Subdivide (Step 47): "Off" (option 0) + the shared note-value palette
    if (index == 7 || index == 9 || index == 14 || index == 16 || index == 18) return (int) sweepModeNames.size(); // Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Delay Time Mode, Mix Mode, Feedback Mode (Flanger)
    if (index == 10) return numNoteValueOptions; // Rate (Scratch v1) -- the shared note-value palette, no "Off" (Scratch always has a rate)
    if (index == 11 || index == 12) return numEasingCurveOptions; // Forward Curve, Backward Curve (Scratch v2)
    if (index == 20) return (int) volumeRampModeNames.size(); // Volume Mode

    return 0;
}

juce::String SlicerAudioProcessor::getSequencerCellParameterOptionName (int index, int optionIndex)
{
    if (index == 1) return getFilterSweepFilterTypeName (optionIndex);
    if (index == 2) return getCurveShapeName (optionIndex);
    if (index == 5) return optionIndex <= 0 ? "Off" : getNoteValueName (optionIndex - 1); // Subdivide (Step 47)
    if (index == 10) return getNoteValueName (optionIndex); // Rate (Scratch v1)
    if (index == 11 || index == 12) return getEasingCurveName (easingCurveFromIndex (optionIndex)); // Forward Curve, Backward Curve (Scratch v2)

    if (index == 7 || index == 9 || index == 14 || index == 16 || index == 18) // Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Delay Time Mode, Mix Mode, Feedback Mode (Flanger)
    {
        if (optionIndex < 0 || optionIndex >= (int) sweepModeNames.size())
            return {};

        return sweepModeNames[(size_t) optionIndex];
    }

    if (index == 20) // Volume Mode
    {
        if (optionIndex < 0 || optionIndex >= (int) volumeRampModeNames.size())
            return {};

        return volumeRampModeNames[(size_t) optionIndex];
    }

    return {};
}

float SlicerAudioProcessor::getSequencerCellParameterMin (int index)
{
    if (index == 0) return minFilterSweepResonance;
    if (index == 3) return minStretchGrainSizeMs;
    if (index == 4) return minStretchSpeedMultiplier;
    if (index == 6) return 1.0f;  // Sample Rate Reduction (Step 49) -- 1 sample hold = no reduction at all
    if (index == 8) return 1.0f;  // Bit Depth (Step 49) -- 1 bit is the quantizer's own hard floor
    if (index == 13) return flangerDelayTimeMinMs; // Delay Time (Flanger) -- least pronounced comb character
    if (index == 15) return 0.0f; // Mix (Flanger) -- fully dry
    if (index == 17) return 0.0f; // Feedback (Flanger) -- no feedback at all
    if (index == 19) return 0.0f; // Volume -- silence
    if (index == 22) return 1.0f; // Delay Bus Time -- same floor setDelayBusTimeMs() itself clamps to

    return 0.0f;
}

float SlicerAudioProcessor::getSequencerCellParameterMax (int index)
{
    if (index == 0) return maxFilterSweepResonance;
    if (index == 3) return maxStretchGrainSizeMs;
    if (index == 4) return maxStretchSpeedMultiplier;
    if (index == 5) return (float) (numNoteValueOptions + 1 - 1); // Subdivide (Step 47) -- unused by the stepped slider itself (see isSequencerCellParameterSteppedSlider), kept for API symmetry
    if (index == 6) return bitcrushRateReductionExtreme; // Sample Rate Reduction (Step 49) -- slider tops out exactly at the Sweep In/Out target, so "Static, maxed out" and "fully swept" read as the same crush amount
    if (index == 8) return 16.0f; // Bit Depth (Step 49) -- a generous ceiling; the sweep's own extreme (1 bit) sits at the MIN end instead, see getSequencerCellParameterMin()
    if (index == 13) return flangerDelayTimeExtremeMs; // Delay Time (Flanger) -- slider tops out exactly at the Sweep In/Out target, same "Static maxed out == fully swept" convention as Sample Rate Reduction
    if (index == 15) return flangerMixExtreme; // Mix (Flanger) -- fully wet, same convention
    if (index == 17) return flangerFeedbackExtreme; // Feedback (Flanger) -- 88%, short of self-oscillation, same convention
    if (index == 19) return 1.0f; // Volume -- full volume/unity gain
    if (index == 22) return 2000.0f; // Delay Bus Time -- same ceiling setDelayBusTimeMs() itself clamps to
    if (index == 23) return 0.95f;   // Delay Bus Feedback -- same ceiling setDelayBusFeedback() itself clamps to
    // 21 (Delay Send Amount), 24 (Reverb Send Amount), 25 (Reverb Bus
    // Size), 26 (Reverb Bus Decay) all fall through to the default 1.0f
    // below, which already matches their own 0..1 range.

    return 1.0f;
}

std::vector<int> SlicerAudioProcessor::getApplicableSequencerCellParameters (int style)
{
    // Parameter indices below match getSequencerCellParameterName()'s own
    // ordering: 0 Resonance, 1 Filter Type, 2 Curve Shape, 3 Grain Size,
    // 4 Grain Speed (Step 45/46), 6 Sample Rate Reduction, 8 Bit Depth
    // (Step 49), 13 Delay Time, 15 Mix, 17 Feedback (Flanger -- their
    // paired Mode indices 7/9/14/16/18 are deliberately never listed
    // here, see isSequencerCellParameterSwept()). Table form rather
    // than a single flat list filtered by style -- keeps "which style
    // offers what" readable at a glance and trivially extensible (a
    // future parameter just adds itself to whichever case(s) it applies
    // to). Style indices match indexToPlaybackStyle()'s ordinals, the
    // same ones sequencerGrid itself stores.
    std::vector<int> params;

    switch (style)
    {
        case 0: break;             // Forward -- no style-specific params, but Subdivide (below) still applies
        case 1: params = { 2 }; break;    // Ping-Pong -- Curve Shape (turnaround fade)
        case 2: params = { 2 }; break;    // Tape Stop -- Curve Shape (decel)
        case 3: params = { 3, 4 }; break; // Stretch -- Grain Size, Grain Speed
        case 4:                           // Filter Down
        case 5: params = { 0, 1 }; break; // Filter Up -- Resonance, Filter Type
        case 6: params = { 6, 8 }; break;      // Bitcrush (Step 49) -- Sample Rate Reduction, Bit Depth
        case 7: params = { 10, 11, 12 }; break; // Scratch (v1/v2) -- Rate, Forward Curve, Backward Curve
        case 8: params = { 13, 15, 17 }; break; // Flanger -- Delay Time, Mix, Feedback
        default: return {};        // empty/invalid cell (-1) -- no menu at all
    }

    // Subdivide (Step 47) and Volume (index 19): both general, not
    // style-specific -- appended for every valid style above (including
    // Forward), unlike everything else in this function. Volume is a
    // pure gain stage layered after whatever the style's own DSP already
    // produces, so it applies identically regardless of which style (if
    // any -- Forward included) is active.
    //
    // Send-to-bus (Pass 2, indices 21-26) is ALSO general/style-independent
    // but is deliberately NOT appended here -- SequencerGrid's right-click
    // menu offers "Send to Delay"/"Send to Reverb" itself, as two grouped
    // submenus, for every active step regardless of what this function
    // returns (see its showParameterMenuForCell()). Keeping them out of
    // this list also keeps them out of buildColumnsForStyle()'s
    // Slice Length/Clock-mode dial panel and randomizeSequence()'s
    // per-style randomization, both of which iterate this function's own
    // result -- neither is meaningful for a per-step bus send.
    params.push_back (5);
    params.push_back (19);
    return params;
}

float SlicerAudioProcessor::getSequencerCellParameterGlobalValue (int index) const
{
    switch (index)
    {
        case 0: return getFilterSweepResonance();
        case 1: return (float) getFilterSweepFilterType();
        case 2: return (float) getCurveShape();
        case 3: return getStretchGrainSizeMs();
        case 4: return getStretchSpeedMultiplier();
        case 5: return 0.0f; // Subdivide (Step 47) -- no global dial; Off is always the fallback/default
        case 6: return getBitcrushRateReductionGlobal(); // Sample Rate Reduction
        case 7: return (float) getBitcrushRateReductionModeGlobal();
        case 8: return getBitcrushBitDepthGlobal(); // Bit Depth
        case 9: return (float) getBitcrushBitDepthModeGlobal();
        case 10: return (float) getScratchRateGlobal(); // Rate (Scratch v1)
        case 11: return (float) getScratchForwardCurveGlobal(); // Forward Curve (Scratch v2)
        case 12: return (float) getScratchBackwardCurveGlobal(); // Backward Curve (Scratch v2)
        case 13: return getFlangerDelayTimeGlobal(); // Delay Time (Flanger)
        case 14: return (float) getFlangerDelayTimeModeGlobal();
        case 15: return getFlangerMixGlobal(); // Mix (Flanger)
        case 16: return (float) getFlangerMixModeGlobal();
        case 17: return getFlangerFeedbackGlobal(); // Feedback (Flanger)
        case 18: return (float) getFlangerFeedbackModeGlobal();
        case 19: return 1.0f; // Volume -- no global dial; full volume (no change) is always the fallback/default
        case 20: return 0.0f; // Volume Mode -- Static is always the fallback/default
        case 21: return 0.0f; // Delay Send Amount -- no global dial (no Send Amount control exists outside a per-step override); 0 (no send) is always the fallback/default
        case 22: return getDelayBusTimeMs(); // Delay Bus Time -- the shared bus's own current (possibly still-ramping) live value
        case 23: return getDelayBusFeedback(); // Delay Bus Feedback -- ditto
        case 24: return 0.0f; // Reverb Send Amount -- no global dial; 0 (no send) is always the fallback/default
        case 25: return getReverbBusSize(); // Reverb Bus Size -- the shared bus's own current live value
        case 26: return getReverbBusDecay(); // Reverb Bus Decay -- ditto
        default: return 0.0f;
    }
}

void SlicerAudioProcessor::setSequencerCellParameterGlobalValue (int index, float value)
{
    switch (index)
    {
        case 0: setFilterSweepResonance (value); break;
        case 1: setFilterSweepFilterType (juce::roundToInt (value)); break;
        case 2: setCurveShape (juce::roundToInt (value)); break;
        case 3: setStretchGrainSizeMs (value); break;
        case 4: setStretchSpeedMultiplier (value); break;
        case 6: setBitcrushRateReductionGlobal (value); break;
        case 7: setBitcrushRateReductionModeGlobal (juce::roundToInt (value)); break;
        case 8: setBitcrushBitDepthGlobal (value); break;
        case 9: setBitcrushBitDepthModeGlobal (juce::roundToInt (value)); break;
        case 10: setScratchRateGlobal (juce::roundToInt (value)); break;
        case 11: setScratchForwardCurveGlobal (juce::roundToInt (value)); break;
        case 12: setScratchBackwardCurveGlobal (juce::roundToInt (value)); break;
        case 13: setFlangerDelayTimeGlobal (value); break;
        case 14: setFlangerDelayTimeModeGlobal (juce::roundToInt (value)); break;
        case 15: setFlangerMixGlobal (value); break;
        case 16: setFlangerMixModeGlobal (juce::roundToInt (value)); break;
        case 17: setFlangerFeedbackGlobal (value); break;
        case 18: setFlangerFeedbackModeGlobal (juce::roundToInt (value)); break;
        default: break; // indices 5 (Subdivide) and 19/20 (Volume/Volume Mode) have no global dial; out-of-range is a no-op
    }
}

bool SlicerAudioProcessor::getSequencerCellHasParameterOverride (int row, int column, const juce::String& parameterName) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return false;

    const auto it = sequencerCellParameterOverrides.find (row * columns + column);
    return it != sequencerCellParameterOverrides.end() && it->second.count (parameterName) > 0;
}

float SlicerAudioProcessor::getSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float fallbackValue) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return fallbackValue;

    const auto it = sequencerCellParameterOverrides.find (row * columns + column);

    if (it == sequencerCellParameterOverrides.end())
        return fallbackValue;

    const auto valueIt = it->second.find (parameterName);
    return valueIt != it->second.end() ? valueIt->second : fallbackValue;
}

void SlicerAudioProcessor::setSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float value)
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return;

    sequencerCellParameterOverrides[row * columns + column][parameterName] = value;
}

bool SlicerAudioProcessor::getSequencerCellHasAnyParameterOverride (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return false;

    const auto it = sequencerCellParameterOverrides.find (row * columns + column);
    return it != sequencerCellParameterOverrides.end() && ! it->second.empty();
}

void SlicerAudioProcessor::randomizeSequence()
{
    clearSequence(); // same wipe Clear Sequence itself uses (Step 41) -- juce::CriticalSection is re-entrant, so re-locking below is safe

    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if (rows <= 0 || columns <= 0)
        return;

    const double stepBeats = getNoteValueBeats (stepResolutionIndex.load());
    const double originalBpm = getCalculatedOriginalBpm();

    // Each row's natural length in steps, computed once up front (Step 40)
    // -- same math SequencerGrid's piano-roll bar uses. Needed per-row on
    // every pass below, so it's not worth recomputing from scratch each time.
    std::vector<int> naturalStepsPerRow ((size_t) rows, 1);

    for (int row = 0; row < rows; ++row)
    {
        const auto& slice = slices[(size_t) row];
        const int sliceLength = slice.endSample - slice.startSample;
        int naturalSteps = 1;

        if (sliceLength > 0 && sampleSampleRate > 0.0 && stepBeats > 0.0 && originalBpm > 0.0)
        {
            const double sliceSeconds = (double) sliceLength / sampleSampleRate;
            const double naturalBeats = sliceSeconds * (originalBpm / 60.0);
            naturalSteps = juce::jmax (1, juce::roundToInt (naturalBeats / stepBeats));
        }

        naturalStepsPerRow[(size_t) row] = naturalSteps;
    }

    // Tracks which columns are already claimed by some previously-placed
    // hit's FULL span, not just its starting column -- what stops a longer
    // bar from getting fragmented by another hit landing mid-span.
    std::vector<bool> columnOccupied ((size_t) columns, false);

    // Fair round-robin placement (Step 40): a single sequential row-by-row
    // sweep let early rows (or simply rows lucky enough to go first)
    // greedily claim most of the grid before later rows ever got a turn --
    // a row that never gets a placement opportunity obviously can't show
    // its own length either, which was the real cause of length-awareness
    // seeming broken for later rows. Instead, run repeated PASSES, each
    // pass offering every row exactly one placement opportunity in a
    // freshly reshuffled order, so no row (and no fixed position within a
    // pass) is systematically favoured. Stops once a whole pass places
    // nothing -- either the grid is full, or every remaining attempt lost
    // its random roll -- capped generously against runaway looping.
    std::vector<int> rowOrder ((size_t) rows);

    for (int i = 0; i < rows; ++i)
        rowOrder[(size_t) i] = i;

    constexpr int maxPasses = 4096;

    for (int pass = 0; pass < maxPasses; ++pass)
    {
        // Fisher-Yates shuffle, reshuffled every pass.
        for (int i = rows - 1; i > 0; --i)
        {
            const int j = random.nextInt (i + 1);
            std::swap (rowOrder[(size_t) i], rowOrder[(size_t) j]);
        }

        bool placedAnythingThisPass = false;

        for (const int row : rowOrder)
        {
            const int naturalSteps = naturalStepsPerRow[(size_t) row];

            // Randomized starting search position (Step 40) -- searching
            // from column 0 every time would otherwise bias placements
            // toward low column indices across the whole grid.
            const int startColumn = random.nextInt (columns);
            int foundColumn = -1;

            for (int offset = 0; offset < columns; ++offset)
            {
                const int column = (startColumn + offset) % columns;
                const int spanEnd = juce::jmin (columns, column + naturalSteps);

                bool spanFree = true;

                for (int c = column; c < spanEnd; ++c)
                {
                    if (columnOccupied[(size_t) c])
                    {
                        spanFree = false;
                        break;
                    }
                }

                if (spanFree)
                {
                    foundColumn = column;
                    break;
                }
            }

            if (foundColumn < 0)
                continue; // no free span anywhere for this row right now

            if (random.nextFloat() < 0.35f)
            {
                // Style (Step 41) is drawn from the same weighted
                // playbackStyleProbabilities table Slice Length/Clock
                // modes already use -- NOT flat/uniform chance -- so
                // turning a style's weight down elsewhere also makes
                // Randomize reach for it less often here. Default weights
                // are Forward-only, so this reproduces exactly the old
                // all-Forward behaviour until those weights are touched.
                const int placedStyle = pickWeightedIndex (playbackStyleProbabilities);
                sequencerGrid[(size_t) (row * columns + foundColumn)] = placedStyle;

                // Per-style "randomize parameters" opt-in (see
                // getRandomizeParametersForStyle()'s own doc comment) --
                // unchecked (the default) leaves this step with no
                // override at all, identical to the behaviour above before
                // this feature existed. Checked rolls an independent
                // random value for every parameter this style actually
                // owns (getApplicableSequencerCellParameters() minus the
                // general Subdivide/Volume entries, same exclusion
                // PlaybackStyleParameterPanel uses), plus an independent
                // random Static/Sweep In/Sweep Out mode for whichever of
                // those are swept.
                if (placedStyle >= 0 && placedStyle < (int) randomizeParametersForStyle.size()
                    && randomizeParametersForStyle[(size_t) placedStyle])
                {
                    for (const int paramIndex : getApplicableSequencerCellParameters (placedStyle))
                    {
                        if (paramIndex == 5 || paramIndex == 19) // Subdivide, Volume -- general, not this style's own
                            continue;

                        const juce::String paramName = getSequencerCellParameterName (paramIndex);

                        // Discrete (Filter Type/Curve Shape/Rate/Forward
                        // Curve/Backward Curve/etc.) picks a random option
                        // index; everything else (Resonance/Grain Size/
                        // Grain Speed, and a swept parameter's own Value)
                        // picks a random point in its min/max range --
                        // isSequencerCellParameterSwept() and
                        // isSequencerCellParameterDiscrete() never overlap.
                        if (isSequencerCellParameterDiscrete (paramIndex))
                        {
                            const int numOptions = getSequencerCellParameterNumOptions (paramIndex);
                            const int randomOption = numOptions > 0 ? random.nextInt (numOptions) : 0;
                            setSequencerCellParameterOverride (row, foundColumn, paramName, (float) randomOption);
                        }
                        else
                        {
                            const float minValue = getSequencerCellParameterMin (paramIndex);
                            const float maxValue = getSequencerCellParameterMax (paramIndex);
                            setSequencerCellParameterOverride (row, foundColumn, paramName, minValue + random.nextFloat() * (maxValue - minValue));
                        }

                        if (isSequencerCellParameterSwept (paramIndex))
                        {
                            const int modeIndex = paramIndex + 1;
                            const int numModes = getSequencerCellParameterNumOptions (modeIndex);
                            const int randomMode = numModes > 0 ? random.nextInt (numModes) : 0;
                            setSequencerCellParameterOverride (row, foundColumn, getSequencerCellParameterName (modeIndex), (float) randomMode);
                        }
                    }
                }

                const int spanEnd = juce::jmin (columns, foundColumn + naturalSteps);

                for (int c = foundColumn; c < spanEnd; ++c)
                    columnOccupied[(size_t) c] = true;

                placedAnythingThisPass = true;
            }
        }

        if (! placedAnythingThisPass)
            break;
    }
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

    rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
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
    auto autoSlices = transientDetector.detectSlices (currentSensitivity.load(), computeMinimumHoldoffMs(), trimStart, trimEnd);

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

    rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SlicerAudioProcessor();
}
