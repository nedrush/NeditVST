#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <unistd.h>

namespace
{
    // Crash diagnostics + file logging (see docs/logging-system.md).
    //
    // NOTE: the signal handler below must stay async-signal-safe, which
    // means it can only use raw POSIX calls (open/write/close) and must
    // NEVER call into JUCE or allocate. The log file path is therefore
    // resolved once at init into a plain static buffer (crashLogPath);
    // the handler just opens whatever's already in there. That also keeps
    // the path portable -- resolved via juce::File::getSpecialLocation so
    // it lands in ~/nedit_crash.log on macOS as well as Linux -- instead
    // of a hardcoded /home/... path that only exists on one machine.
    std::ofstream logFile;
    std::mutex logMutex;
    std::array<char, 1024> crashLogPath;

    void neditLog (const char* msg)
    {
        const std::lock_guard<std::mutex> lock (logMutex);

        if (! logFile.is_open())
        {
            auto path = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                            .getChildFile ("nedit_crash.log");
            logFile.open (path.getFullPathName().toRawUTF8(), std::ios::app);
        }

        // timestamp
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (now.time_since_epoch()).count();
        logFile << "[" << ms << "] " << msg << "\n";
        logFile.flush();
    }

    void initCrashLogPath()
    {
        auto path = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                        .getChildFile ("nedit_crash.log")
                        .getFullPathName();

        std::memset (crashLogPath.data(), 0, crashLogPath.size());
        std::strncpy (crashLogPath.data(), path.toRawUTF8(), crashLogPath.size() - 1);
    }

    void neditLogSignalHandler (int sig)
    {
        const char* sigName = "UNKNOWN";
        if (sig == SIGSEGV) sigName = "SIGSEGV";
        else if (sig == SIGABRT) sigName = "SIGABRT";
        else if (sig == SIGFPE) sigName = "SIGFPE";

        // Use only async-signal-safe functions (write, open, close).
        // Also write to the crash log file so the backtrace is easy to find.
        const int logFd = open (crashLogPath.data(), O_WRONLY | O_CREAT | O_APPEND, 0644);

        auto safeWrite = [] (int fd, const char* s, size_t len)
        {
            size_t total = 0;
            while (total < len)
            {
                ssize_t n = write (fd, s + total, len - total);
                if (n <= 0) break;
                total += (size_t) n;
            }
        };

        const char header[] = "*** SIGNAL / CRASH: ";
        const char trailer[] = " ***\n";

        if (logFd >= 0)
        {
            safeWrite (logFd, header, sizeof(header) - 1);
            safeWrite (logFd, sigName, strlen (sigName));
            safeWrite (logFd, trailer, sizeof(trailer) - 1);
        }

        // Also write to stderr, which hosts like Bitwig capture in their
        // engine log.
        safeWrite (STDERR_FILENO, header, sizeof(header) - 1);
        safeWrite (STDERR_FILENO, sigName, strlen (sigName));
        safeWrite (STDERR_FILENO, trailer, sizeof(trailer) - 1);

        void* callstack[64];
        const int frames = backtrace (callstack, 64);

        // Write backtrace to both stderr and the log file
        backtrace_symbols_fd (callstack, frames, STDERR_FILENO);
        if (logFd >= 0)
        {
            backtrace_symbols_fd (callstack, frames, logFd);
            safeWrite (logFd, "\n", 1);
            close (logFd);
        }

        ::_exit (1);
    }

    struct NeditLogInit
    {
        NeditLogInit()
        {
            initCrashLogPath();
            neditLog ("--- plugin loaded ---");
            ::signal (SIGSEGV, neditLogSignalHandler);
            ::signal (SIGABRT, neditLogSignalHandler);
            ::signal (SIGFPE,  neditLogSignalHandler);
        }
    } neditLogInit;

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

    // Bitcrush character constants (Step 48/49). Defaults match the
    // original Step 48 fixed constants exactly (12-sample hold, 5-bit
    // depth), used whenever no per-step override exists (Slice Length/
    // Clock modes always, Sequenced mode steps that don't customize it) --
    // same "no global dial, fixed fallback" precedent as Subdivide.
    // Extremes are Sweep In/Out's fixed far end (Step 49) -- 48 samples is
    // roughly 1/48th of the original rate (well past the original spec's
    // 1/8-1/16 default range, deliberately more extreme since a SWEEP
    // TARGET is meant to read as "maximally crushed," not "typical");
    // 1 bit is the most crushed a bit-depth quantizer can go.
    constexpr float bitcrushRateReductionDefault = 12.0f;
    constexpr float bitcrushRateReductionExtreme = 48.0f;
    constexpr float bitcrushBitDepthDefault = 5.0f;
    constexpr float bitcrushBitDepthExtreme = 1.0f;

    // Flanger character constants -- same "fixed default, fixed Sweep
    // In/Out extreme, no global dial" shape as Bitcrush's own constants
    // just above. Delay Time's extreme sits at the TOP of its range (10ms,
    // the deepest/most pronounced comb notch) exactly like Sample Rate
    // Reduction's extreme does -- "Static, maxed out" and "fully swept"
    // read as the same amount of character. Mix's extreme is fully wet
    // (1.0) for the same reason -- it's the far end of the 0-100% range
    // the parameter is already documented against. Feedback's extreme
    // (0.88) is deliberately short of the mathematically-stable 1.0 ceiling
    // -- comfortably clear of self-oscillation/runaway buildup while still
    // landing on a genuinely resonant, pronounced comb character at
    // "Static, maxed out"/Sweep's far end, same convention as the other
    // two.
    constexpr float flangerDelayTimeMinMs = 0.5f;
    constexpr float flangerDelayTimeDefaultMs = 2.0f;
    constexpr float flangerDelayTimeExtremeMs = 10.0f;
    constexpr float flangerMixDefault = 0.5f;
    constexpr float flangerMixExtreme = 1.0f;
    constexpr float flangerFeedbackDefault = 0.3f;
    constexpr float flangerFeedbackExtreme = 0.88f;

    // Scratch's default Rate (v1) -- index into the shared note-value
    // palette below, used whenever no per-step override exists (Slice
    // Length/Clock modes always; Sequenced mode steps that don't
    // customize it), same "no global dial, fixed fallback" precedent as
    // Subdivide/Bitcrush above. Index 7 is 16n -- a fast default, and the
    // same index stepResolutionIndex itself defaults to.
    constexpr int scratchDefaultRateIndex = 7;

    // Sweep mode names (Step 49), shared by every swept parameter's own
    // Mode index -- Bitcrush's Sample Rate Reduction Mode/Bit Depth Mode,
    // and Flanger's Delay Time Mode/Mix Mode -- see SlicerAudioProcessor::
    // isSequencerCellParameterSwept().
    const std::array<const char*, 3> sweepModeNames { {
        "Static", "Sweep In", "Sweep Out"
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
        "Feedback", "Feedback Mode"
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
    neditLog ("Processor ctor");
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
}

SlicerAudioProcessor::~SlicerAudioProcessor() { neditLog ("Processor dtor"); }

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

    // Log if the audio thread had to wait more than 1ms for sampleLock
    // (i.e., the message thread was holding it too long).
    static int processBlockCount = 0;
    if (++processBlockCount % 500 == 0) // every ~11ms at 44.1kHz/512
    {
        const auto before = juce::Time::getHighResolutionTicks();
        const bool got = sampleLock.tryEnter();
        const auto after = juce::Time::getHighResolutionTicks();
        const double waitMs = (double) (after - before) * 1000.0 / (double) juce::Time::getHighResolutionTicksPerSecond();

        if (got)
        {
            sampleLock.exit();
            if (waitMs > 1.0)
            {
                char buf[128];
                snprintf (buf, sizeof buf, "processBlock: lock wait %.2f ms", waitMs);
                neditLog (buf);
            }
        }
        else
        {
            neditLog ("processBlock: tryEnter FAILED");
        }
    }

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
            sequencedSubdivisionActive = false; // Step 47 -- no stale retrigger schedule carried in from before this mode was (re-)entered
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

                    // Scratch (v1) -- Clock mode always uses the fixed
                    // default Rate; per-step overrides are Sequenced-mode-
                    // only, same pattern as Bitcrush above. Captured here,
                    // before currentEndSample below, since it feeds
                    // directly into that calculation, same as Grain Speed
                    // does for Stretch.
                    currentPickScratchCycleLengthHostSamples = scratch
                        ? computeScratchCycleLengthHostSamples (scratchDefaultRateIndex, currentSliceLength,
                                                                 hostBpm, hostSampleRate, playbackRate)
                        : 0.0;

                    // Forward/Backward Curve (Scratch v2) -- Clock mode
                    // always uses the fixed Linear/Linear default; per-step
                    // overrides are Sequenced-mode-only, same pattern as
                    // Rate just above.
                    currentPickScratchForwardCurve = EasingCurve::linear;
                    currentPickScratchBackwardCurve = EasingCurve::linear;

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

                    // Filter Sweep resonance/filter type + Curve Shape
                    // (Step 45/46) -- Clock mode always uses the global
                    // values; per-step overrides are Sequenced-mode-only.
                    currentPickFilterSweepResonance = filterSweepResonanceValue.load();
                    currentPickFilterSweepType = filterSweepFilterTypeValue.load();
                    currentPickCurveShape = curveShapeValue.load();

                    // Bitcrush Sample Rate Reduction/Bit Depth (Step 49) --
                    // Clock mode always uses the fixed default at Static
                    // mode; per-step value/mode overrides are Sequenced-
                    // mode-only, same pattern as Filter Sweep just above.
                    currentPickBitcrushRateValue = bitcrushRateReductionDefault;
                    currentPickBitcrushRateMode = 0;
                    currentPickBitcrushBitDepthValue = bitcrushBitDepthDefault;
                    currentPickBitcrushBitDepthMode = 0;

                    // Flanger Delay Time/Mix/Feedback -- Clock mode always
                    // uses the fixed default at Static mode; per-step
                    // value/mode overrides are Sequenced-mode-only, same
                    // pattern as Bitcrush just above.
                    currentPickFlangerDelayValue = flangerDelayTimeDefaultMs;
                    currentPickFlangerDelayMode = 0;
                    currentPickFlangerMixValue = flangerMixDefault;
                    currentPickFlangerMixMode = 0;
                    currentPickFlangerFeedbackValue = flangerFeedbackDefault;
                    currentPickFlangerFeedbackMode = 0;

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
                            activeRow, currentStepIndex, "Sample Rate Reduction", bitcrushRateReductionDefault);
                        currentPickBitcrushRateMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Sample Rate Reduction Mode", 0.0f));
                        currentPickBitcrushBitDepthValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Bit Depth", bitcrushBitDepthDefault);
                        currentPickBitcrushBitDepthMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Bit Depth Mode", 0.0f));

                        // Flanger Delay Time/Mix/Feedback VALUE + MODE --
                        // same per-step-override-else-global pattern as
                        // Bitcrush just above.
                        currentPickFlangerDelayValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Delay Time", flangerDelayTimeDefaultMs);
                        currentPickFlangerDelayMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Delay Time Mode", 0.0f));
                        currentPickFlangerMixValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Mix", flangerMixDefault);
                        currentPickFlangerMixMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Mix Mode", 0.0f));
                        currentPickFlangerFeedbackValue = getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Feedback", flangerFeedbackDefault);
                        currentPickFlangerFeedbackMode = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Feedback Mode", 0.0f));

                        // Scratch Rate (v1) -- this step's own override if
                        // it has one, else the fixed default, same per-
                        // step-override-else-global pattern as Filter
                        // Sweep/Bitcrush above.
                        const int scratchRateIndex = juce::roundToInt (getSequencerCellParameterOverride (
                            activeRow, currentStepIndex, "Rate", (float) scratchDefaultRateIndex));
                        currentPickScratchCycleLengthHostSamples = scratch
                            ? computeScratchCycleLengthHostSamples (scratchRateIndex, currentSliceLength,
                                                                     hostBpm, hostSampleRate, playbackRate)
                            : 0.0;

                        // Forward/Backward Curve (Scratch v2) -- this
                        // step's own override if it has one, else Linear
                        // (v1's exact constant-speed behaviour), same
                        // per-step-override-else-fixed-default pattern as
                        // Rate just above.
                        currentPickScratchForwardCurve = easingCurveFromIndex (juce::roundToInt (
                            getSequencerCellParameterOverride (activeRow, currentStepIndex, "Forward Curve", 0.0f)));
                        currentPickScratchBackwardCurve = easingCurveFromIndex (juce::roundToInt (
                            getSequencerCellParameterOverride (activeRow, currentStepIndex, "Backward Curve", 0.0f)));

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

                // Scratch (v1) -- Slice Length mode always uses the fixed
                // default Rate; per-step overrides are Sequenced-mode-only,
                // same pattern as Bitcrush above. Captured here, before
                // currentEndSample below, since it feeds directly into that
                // calculation, same as Grain Speed does for Stretch.
                currentPickScratchCycleLengthHostSamples = scratch
                    ? computeScratchCycleLengthHostSamples (scratchDefaultRateIndex, currentSliceLength,
                                                             hostBpm, hostSampleRate, playbackRate)
                    : 0.0;

                // Forward/Backward Curve (Scratch v2) -- Slice Length mode
                // always uses the fixed Linear/Linear default; per-step
                // overrides are Sequenced-mode-only, same pattern as Rate
                // just above.
                currentPickScratchForwardCurve = EasingCurve::linear;
                currentPickScratchBackwardCurve = EasingCurve::linear;

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
                // Slice Length mode always uses the fixed default at
                // Static mode; per-step value/mode overrides are
                // Sequenced-mode-only, same pattern as Filter Sweep above.
                currentPickBitcrushRateValue = bitcrushRateReductionDefault;
                currentPickBitcrushRateMode = 0;
                currentPickBitcrushBitDepthValue = bitcrushBitDepthDefault;
                currentPickBitcrushBitDepthMode = 0;

                // Flanger Delay Time/Mix/Feedback -- Slice Length mode
                // always uses the fixed default at Static mode; per-step
                // value/mode overrides are Sequenced-mode-only, same
                // pattern as Bitcrush just above.
                currentPickFlangerDelayValue = flangerDelayTimeDefaultMs;
                currentPickFlangerDelayMode = 0;
                currentPickFlangerMixValue = flangerMixDefault;
                currentPickFlangerMixMode = 0;
                currentPickFlangerFeedbackValue = flangerFeedbackDefault;
                currentPickFlangerFeedbackMode = 0;

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
                ? (currentPickScratchCycleLengthHostSamples * 0.5 * playbackRate)
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

            if (timeStretchMode || stretchActive)
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
                    grainSourceHopSamples = grainOutputHopSamples * (playbackRate / (double) currentPickStretchSpeedMultiplier);

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
        // Sequenced mode (Step 47) also needs this incrementing -- it's
        // reused there as the Subdivide retrigger scheduler's own clock
        // (see the sequencedMode branch above), not just the Filter Sweep
        // Whole Window progress fraction.
        if (clockMode || sequencedMode)
            samplesSinceWindowStart += 1.0;
    }
}

juce::AudioProcessorEditor* SlicerAudioProcessor::createEditor()
{
    neditLog ("createEditor");
    return new SlicerAudioProcessorEditor (*this);
}

void SlicerAudioProcessor::getStateInformation (juce::MemoryBlock& /*destData*/)
{
    neditLog ("getStateInformation");
    // TODO once there are parameters worth persisting (loop length, slice
    // probabilities) — not wired up yet in this step.
}

void SlicerAudioProcessor::setStateInformation (const void* /*data*/, int /*sizeInBytes*/)
{
    neditLog ("setStateInformation: begin");
    neditLog ("setStateInformation: done");
}

void SlicerAudioProcessor::loadSample (const juce::File& file)
{
    neditLog (("loadSample: " + file.getFullPathName()).toRawUTF8());
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
        return;

    juce::AudioBuffer<float> newBuffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&newBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

    {
        char buf[256];
        snprintf (buf, sizeof buf, "loadSample: read %d samples, %d ch, %.1f MB",
                  (int) reader->lengthInSamples, (int) reader->numChannels,
                  (float) reader->lengthInSamples * reader->numChannels * 4.0f / (1024.0f * 1024.0f));
        neditLog (buf);
    }

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

    // TEMPORARY (Onset vs. Peak comparison tool): this is the ONE place the
    // toggle actually decides what gets played -- see setUseOnsetDetection().
    const auto method = useOnsetDetectionForPlayback.load() ? DetectionMethod::onset
                                                             : DetectionMethod::peak;
    auto autoSlices = transientDetector.detectSlices (sensitivity, holdoffMs, trimStart, trimEnd, method);

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

    // TEMPORARY (Onset vs. Peak comparison tool): preview whichever method
    // is actually driving playback, so a live sensitivity drag matches what
    // will get committed. See rebuildSlicesFromDetectionAndManualPoints().
    const auto method = useOnsetDetectionForPlayback.load() ? DetectionMethod::onset
                                                             : DetectionMethod::peak;
    auto autoSlices = transientDetector.detectSlices (sensitivity, defaultHoldoffMs, trimStart, trimEnd, method);

    const juce::ScopedLock sl (sampleLock);
    return mergeOnsetsIntoSlices (autoSlices, trimStart, trimEnd);
}

// TEMPORARY (Onset vs. Peak comparison tool -- see the public section this
// pairs with in PluginProcessor.h). Raw, unmerged detector output for the
// waveform's comparison overlay only.
std::vector<int> SlicerAudioProcessor::getPeakDetectionMarkers() const
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    auto rawSlices = transientDetector.detectSlices (currentSensitivity.load(), defaultHoldoffMs, trimStart, trimEnd,
                                                      DetectionMethod::peak);

    std::vector<int> markers;
    markers.reserve (rawSlices.size());
    for (const auto& s : rawSlices)
        markers.push_back (s.startSample);
    return markers;
}

std::vector<int> SlicerAudioProcessor::getOnsetDetectionMarkers() const
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    auto rawSlices = transientDetector.detectSlices (currentSensitivity.load(), defaultHoldoffMs, trimStart, trimEnd,
                                                      DetectionMethod::onset);

    std::vector<int> markers;
    markers.reserve (rawSlices.size());
    for (const auto& s : rawSlices)
        markers.push_back (s.startSample);
    return markers;
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
    sequencerGrid.assign ((size_t) juce::jmax (0, rows * columns), -1);
    sequencerCellParameterOverrides.clear(); // Step 45 -- dimensions just changed, old flat indices are meaningless now
    sequencerCellExtendedLengthSteps.clear(); // Step-extension (Pass 1) -- same reasoning
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
    return index == 1 || index == 2 || index == 5 || index == 7 || index == 9 || index == 10 || index == 11 || index == 12 || index == 14 || index == 16 || index == 18; // Filter Type, Curve Shape (Step 46); Subdivide (Step 47); Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Rate, Forward Curve, Backward Curve (Scratch v1/v2); Delay Time Mode, Mix Mode, Feedback Mode (Flanger)
}

bool SlicerAudioProcessor::isSequencerCellParameterSteppedSlider (int index)
{
    return index == 5; // Subdivide (Step 47) -- see its declaration in PluginProcessor.h for why
}

bool SlicerAudioProcessor::isSequencerCellParameterSwept (int index)
{
    return index == 6 || index == 8 || index == 13 || index == 15 || index == 17; // Sample Rate Reduction, Bit Depth (Step 49); Delay Time, Mix, Feedback (Flanger) -- see declaration in PluginProcessor.h
}

int SlicerAudioProcessor::getSequencerCellParameterNumOptions (int index)
{
    if (index == 1) return numFilterSweepFilterTypeOptions;
    if (index == 2) return numCurveShapeOptions;
    if (index == 5) return numNoteValueOptions + 1; // Subdivide (Step 47): "Off" (option 0) + the shared note-value palette
    if (index == 7 || index == 9 || index == 14 || index == 16 || index == 18) return (int) sweepModeNames.size(); // Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Delay Time Mode, Mix Mode, Feedback Mode (Flanger)
    if (index == 10) return numNoteValueOptions; // Rate (Scratch v1) -- the shared note-value palette, no "Off" (Scratch always has a rate)
    if (index == 11 || index == 12) return numEasingCurveOptions; // Forward Curve, Backward Curve (Scratch v2)

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

    // Subdivide (Step 47): general, not style-specific -- appended for
    // every valid style above (including Forward), unlike everything
    // else in this function.
    params.push_back (5);
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
        case 6: return bitcrushRateReductionDefault; // Sample Rate Reduction (Step 49) -- no global dial; the fixed default is always the fallback
        case 7: return 0.0f; // Sample Rate Reduction Mode (Step 49) -- no global dial; Static is always the fallback/default
        case 8: return bitcrushBitDepthDefault; // Bit Depth (Step 49) -- no global dial; the fixed default is always the fallback
        case 9: return 0.0f; // Bit Depth Mode (Step 49) -- no global dial; Static is always the fallback/default
        case 10: return (float) scratchDefaultRateIndex; // Rate (Scratch v1) -- no global dial; the fixed default is always the fallback
        case 11: return 0.0f; // Forward Curve (Scratch v2) -- no global dial; Linear is always the fallback/default
        case 12: return 0.0f; // Backward Curve (Scratch v2) -- no global dial; Linear is always the fallback/default
        case 13: return flangerDelayTimeDefaultMs; // Delay Time (Flanger) -- no global dial; the fixed default is always the fallback
        case 14: return 0.0f; // Delay Time Mode (Flanger) -- no global dial; Static is always the fallback/default
        case 15: return flangerMixDefault; // Mix (Flanger) -- no global dial; the fixed default is always the fallback
        case 16: return 0.0f; // Mix Mode (Flanger) -- no global dial; Static is always the fallback/default
        case 17: return flangerFeedbackDefault; // Feedback (Flanger) -- no global dial; the fixed default is always the fallback
        case 18: return 0.0f; // Feedback Mode (Flanger) -- no global dial; Static is always the fallback/default
        default: return 0.0f;
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
                sequencerGrid[(size_t) (row * columns + foundColumn)] = pickWeightedIndex (playbackStyleProbabilities);

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
    //
    // TEMPORARY (Onset vs. Peak comparison tool): search whichever method is
    // actually driving playback, so exclusion targets the boundaries the
    // user can actually see/hear active on the waveform right now.
    const auto method = useOnsetDetectionForPlayback.load() ? DetectionMethod::onset
                                                             : DetectionMethod::peak;
    auto autoSlices = transientDetector.detectSlices (currentSensitivity.load(), defaultHoldoffMs, trimStart, trimEnd, method);

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
