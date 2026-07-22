#pragma once

#include <JuceHeader.h>
#include "TransientDetector.h"
#include "GranularStretcher.h"

//==============================================================================
// STEP 6/8: transport-synced generative playback — no MIDI, no keyboard.
//
// How it works (Step 8 revision — self-chaining weighted playback):
//   - The loaded sample is treated as `loopLengthBars` bars long (set by the
//     user). That, plus the sample's actual length, gives us its original
//     tempo, which is repitched (varispeed — pitch follows speed) to match
//     whatever tempo the host is running at.
//   - There is no fixed clock grid. Instead: pick one slice via weighted
//     random draw (weights = the per-slice probability sliders), play it in
//     full at its own (repitched) length, and the INSTANT it finishes, pick
//     again. A slice's own duration is what paces the next decision — a
//     slice with the only nonzero weight simply repeats back-to-back.
//   - This runs continuously the whole time the host transport is playing;
//     there's no bar-boundary resync. Weight 0 = that slice is excluded
//     from the draw entirely (never picked, though the math still tolerates
//     it fine even without exclusion).
//==============================================================================

//==============================================================================
class SlicerAudioProcessor : public juce::AudioProcessor
{
public:
    SlicerAudioProcessor();
    ~SlicerAudioProcessor() override;

    //=== Standard AudioProcessor overrides ===
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; } // transport-driven now, no MIDI needed
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //=== Sample loading (called from the editor) ===
    void loadSample (const juce::File& file);
    bool hasSample() const { return sampleLoaded; }
    juce::String getLoadedFileName() const { return loadedFileName; }
    const juce::AudioBuffer<float>& getSampleBuffer() const { return sampleBuffer; }

    //=== Slicing ===
    void redetectSlices (float sensitivity, float holdoffMs);
    int getNumSlices() const { return (int) slices.size(); }
    Slice getSlice (int index) const { return slices[(size_t) index]; }
    const std::vector<Slice>& getSlices() const { return slices; }

    // Live sensitivity control — was hardcoded until now. Re-runs detection
    // immediately (cheap, since TransientDetector caches the expensive
    // envelope/derivative pass) and resets slice probabilities to 1.0, same
    // as any other re-slice, since the slice boundaries themselves change.
    void setSensitivityAndRedetect (float sensitivity)
    {
        currentSensitivity.store (juce::jlimit (0.0f, 1.0f, sensitivity));
        redetectSlices (currentSensitivity.load(), defaultHoldoffMs);
    }

    float getSensitivity() const { return currentSensitivity.load(); }

    //=== Trim markers (Step 23/25) ===
    // Two independent boundaries, in source-sample units, confining
    // EVERYTHING else in this class to [trimStart, trimEnd): transient
    // detection, manual slice point add/move (including the snap-to-
    // transient search), and therefore what can ever become a slice or get
    // played. Default to the full sample length on load (start=0,
    // end=buffer length), so behaviour is unchanged until the user actually
    // drags a handle. Continuous parameters like sensitivity/loop length —
    // deliberately NOT undo-tracked (see the Undo/redo section below) —
    // dragging a handle just re-triggers the same rebuild pathway sensitivity
    // changes already use, which naturally drops any existing slice boundary
    // (manual or auto) that now falls outside the new range.
    int getTrimStartSample() const { return trimStartSample.load(); }
    int getTrimEndSample() const { return trimEndSample.load(); }

    // Snapping (Step 25) reuses the exact same mechanism manual slice
    // points already use — TransientDetector::findNearestPeak, with Shift
    // held (snapToTransient = false) bypassing it for free placement — no
    // new interaction pattern. One deliberate scoping difference: manual
    // points' snap search is confined to the current trim window, but a
    // trim handle can't use that same constraint (there's no "inside the
    // trim" yet until the trim itself is set), so this searches the WHOLE
    // file's cached transient data, unconstrained — findNearestPeak's
    // default (-1, -1) range args already mean exactly that. The raw
    // target is clamped to the allowed handle range (guarding against the
    // two handles crossing) both before AND after the snap search, since
    // an unconstrained search can land a peak right at — or past — that
    // boundary.
    void setTrimStartSample (int sample, bool snapToTransient = true)
    {
        const int currentEnd = trimEndSample.load();
        const int upperBound = juce::jmax (0, currentEnd - minTrimGapSamples); // guards tiny/degenerate buffers
        int target = juce::jlimit (0, upperBound, sample);

        if (snapToTransient)
        {
            const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
            target = juce::jlimit (0, upperBound, transientDetector.findNearestPeak (target, radiusSamples));
        }

        trimStartSample.store (target);
        rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), defaultHoldoffMs);
    }

    void setTrimEndSample (int sample, bool snapToTransient = true)
    {
        const int currentStart = trimStartSample.load();
        const int bufferLength = sampleBuffer.getNumSamples();
        const int lowerBound = juce::jmin (currentStart + minTrimGapSamples, bufferLength); // guards tiny/degenerate buffers
        int target = juce::jlimit (lowerBound, bufferLength, sample);

        if (snapToTransient)
        {
            const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
            target = juce::jlimit (lowerBound, bufferLength, transientDetector.findNearestPeak (target, radiusSamples));
        }

        trimEndSample.store (target);
        rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), defaultHoldoffMs);
    }

    //=== Audition (Step 25) ===
    // Plays [trimStart, trimEnd) on a tight raw loop at native pitch/speed
    // — sample-rate-matched only (no repitch, no fades, no slicing/picks/
    // probability), completely bypassing the generative engine below, so
    // what you hear is exactly the source content — for counting bars by
    // ear before committing to a loop length. Deliberately unfaded at the
    // loop seam: a click there IS the diagnostic ("not tight yet"), not a
    // defect to smooth over.
    //
    // Works independent of host transport — it has to run whether or not
    // the DAW is playing, since setting up a trim happens before worrying
    // about sync — and auto-stops the instant host transport starts
    // playing, so audition and the real engine never talk over each
    // other. Click Audition again to stop manually if the transport isn't
    // running. See processBlock()'s auditionActive check, which runs
    // before (and instead of) everything below it.
    void setAuditionActive (bool active)
    {
        const juce::ScopedLock sl (sampleLock); // guards auditionPosition, same lock processBlock uses
        if (active)
            auditionPosition = (double) trimStartSample.load(); // always start fresh from the current trim, not wherever a stale position was left
        auditionActive.store (active);
    }

    bool getAuditionActive() const { return auditionActive.load(); }

    // Live preview (Step 12): shows what detection WOULD produce at a
    // given sensitivity — merged with the current manual/excluded points,
    // same as a real commit — but without touching playback state at all
    // (no probability reset, no interrupting the current pick, not added
    // to undo history). Safe to call repeatedly while a slider is being
    // dragged; the real commit only happens via setSensitivityAndRedetect().
    std::vector<Slice> previewSlicesAtSensitivity (float sensitivity) const;

    //=== Manual slice points (Step 10) ===
    // User-placed slice boundaries, layered on top of whatever the
    // detector finds automatically. Unlike auto-detected slices, these
    // survive a sensitivity change — redetection only regenerates the
    // auto side and re-merges it with whatever manual points already
    // exist. Each point snaps to the nearest real transient-like peak in
    // the cached derivative curve (via TransientDetector::findNearestPeak),
    // even one below the current sensitivity threshold.
    struct ManualPointInfo
    {
        int id = -1;
        int samplePosition = 0;
    };

    // Adds a new manual point near targetSample. Snaps to the nearest
    // real transient-like peak by default; pass snapToTransient = false
    // (Shift held) to place it at the exact position instead. Returns its
    // stable id, used later to move or remove it.
    int addManualSlicePoint (int targetSample, bool snapToTransient = true);

    // Moves an existing manual point (by id) to a new target. Snaps by
    // default; pass snapToTransient = false (Shift held) for free
    // placement at the exact position. Deliberately NOT undo-tracked —
    // this is called continuously while the user drags a point, and we
    // don't want one undo step per pixel. Call commitManualPointMove()
    // once, at drag-end, to record the whole drag as a single undoable
    // step.
    void moveManualSlicePoint (int id, int targetSample, bool snapToTransient = true);

    // Records a completed drag (from originalSamplePosition to wherever
    // the point currently is) as one undo step. Call this on mouse-up.
    void commitManualPointMove (int id, int originalSamplePosition);

    void removeManualSlicePoint (int id);

    std::vector<ManualPointInfo> getManualSlicePoints() const
    {
        const juce::ScopedLock sl (sampleLock);
        std::vector<ManualPointInfo> result;
        result.reserve (manualPoints.size());

        for (const auto& mp : manualPoints)
            result.push_back ({ mp.id, mp.samplePosition });

        return result;
    }

    //=== Deleting auto-detected transients (Step 11) ===
    // "Deletes" the nearest auto-detected boundary to targetSample by
    // adding it to an exclusion list — matched by proximity (same
    // tolerance as manual-point snapping) rather than exact position, so
    // a later sensitivity tweak that shifts the detected position by a
    // sample or two doesn't silently un-exclude it. Position 0 (the very
    // start of the sample) can never be excluded. Returns the new
    // exclusion's id, or -1 if there was nothing nearby to exclude.
    int excludeNearestAutoPoint (int targetSample);

    // Un-deletes a single excluded point.
    void restoreExcludedPoint (int id);

    std::vector<ManualPointInfo> getExcludedPoints() const
    {
        const juce::ScopedLock sl (sampleLock);
        std::vector<ManualPointInfo> result;
        result.reserve (excludedPoints.size());

        for (const auto& ep : excludedPoints)
            result.push_back ({ ep.id, ep.samplePosition });

        return result;
    }

    // Safety net: clears every manual addition AND every exclusion in one
    // go, back to exactly what the detector alone would produce at the
    // current sensitivity. Undo-tracked like everything else in this
    // section — one Undo click brings it all back if this was a mistake.
    void resetAllManualEdits();

    //=== Undo/redo (Step 12) ===
    // Covers manual point add/move/remove and auto-point exclude/restore
    // (including Reset) — every slice-editing action, as one coalesced
    // step each. Deliberately does NOT cover sensitivity, probability
    // sliders, loop length, or fades — those are continuous parameters,
    // not discrete "actions," and including them would flood the history
    // with noise from every pixel of a drag.
    bool undoLastEdit() { return undoManager.undo(); }
    bool redoLastEdit() { return undoManager.redo(); }
    bool canUndoEdit() const { return undoManager.canUndo(); }
    bool canRedoEdit() const { return undoManager.canRedo(); }

    // Overwrites manual + excluded point state wholesale and rebuilds —
    // the one place all undo/redo actions actually apply a snapshot.
    // Public because the undo action objects (defined in the .cpp) need
    // to call it; not intended to be called directly from the UI.
    void applyManualState (const std::vector<ManualPointInfo>& manual,
                            const std::vector<ManualPointInfo>& excluded);

    //=== Currently-playing slice (Step 11) ===
    // For UI highlighting — which slice is sounding right now, updated by
    // the audio thread every time a new pick begins. -1 when nothing's
    // playing (including whenever the transport is stopped).
    int getCurrentlyPlayingSliceIndex() const { return currentlyPlayingSliceIndexForUI.load(); }

    //=== Loop length / tempo sync ===
    // How many bars (assumed 4/4) the loaded sample represents. This is
    // what lets us calculate the sample's original tempo and therefore how
    // much to repitch it to match the host.
    void setLoopLengthBars (int bars) { loopLengthBars.store (juce::jmax (1, bars)); }
    int getLoopLengthBars() const { return loopLengthBars.load(); }

    //=== Manual BPM override (Step 23) ===
    // When enabled, REPLACES the bars-derived tempo calculation entirely
    // (not layered alongside it) — see computeSourceSpanSeconds(), the one
    // shared function both this and the Trim markers above feed into, used
    // consistently by processBlock()'s direct-read path and by whatever
    // GranularStretcher renders (via the same repitchRatio it already
    // flows through).
    void setManualBpmOverrideEnabled (bool enabled) { manualBpmOverrideEnabled.store (enabled); }
    bool getManualBpmOverrideEnabled() const { return manualBpmOverrideEnabled.load(); }

    void setManualBpmOverrideValue (double bpm) { manualBpmOverrideValue.store (juce::jmax (1.0, bpm)); }
    double getManualBpmOverrideValue() const { return manualBpmOverrideValue.load(); }

    // Calculated from loopLengthBars + (the trimmed span of the sample, or
    // the manual BPM override when enabled). Exposed mainly so the editor
    // can display it — "this loop is ~140 BPM". Shows the override value
    // directly when it's active, rather than a value re-derived from it
    // (those are mathematically the same number for the *source* span, but
    // showing the raw override avoids any rounding-trip confusion).
    double getCalculatedOriginalBpm() const
    {
        if (manualBpmOverrideEnabled.load())
            return manualBpmOverrideValue.load();

        if (! sampleLoaded || sampleBuffer.getNumSamples() == 0)
            return 0.0;

        const double lengthSeconds = computeSourceSpanSeconds();

        if (lengthSeconds <= 0.0)
            return 0.0;

        const double beats = (double) loopLengthBars.load() * 4.0; // assumes 4/4
        return (beats * 60.0) / lengthSeconds;
    }

    //=== Per-slice weight (Step 8) ===
    // Relative weight in the weighted-random draw that picks the next
    // slice to play — NOT an independent per-hit probability anymore.
    // 0 = excluded from the draw entirely. Higher = more likely relative
    // to the other slices' weights. Defaults to 1.0 (even odds across all
    // slices) on every re-slice.
    float getSliceProbability (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) sliceProbabilities.size())
            return 1.0f;

        return sliceProbabilities[(size_t) index];
    }

    void setSliceProbability (int index, float probability)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) sliceProbabilities.size())
            sliceProbabilities[(size_t) index] = juce::jlimit (0.0f, 1.0f, probability);
    }

    //=== De-clicking (Step 9) ===
    // Global fade-in/fade-out applied at the start/end of every slice
    // pick, in milliseconds — real time, independent of repitching, so a
    // slice played twice as fast still gets the same-length fade. Clamped
    // per-pick to at most half that pick's own length so a very short
    // slice can't have overlapping/inverted fades. Feedback from the
    // original Nedit device was that vocal material especially clicked
    // without this; drum/percussion material is the main target here but
    // the fix is free either way.
    void setFadeInMs (float ms) { fadeInMs.store (juce::jmax (0.0f, ms)); }
    float getFadeInMs() const { return fadeInMs.load(); }

    void setFadeOutMs (float ms) { fadeOutMs.store (juce::jmax (0.0f, ms)); }
    float getFadeOutMs() const { return fadeOutMs.load(); }

    //=== Trigger mode (Step 14) ===
    // Two mutually exclusive ways to decide when the next slice-pick
    // happens:
    //   sliceLength — today's behaviour, unchanged: the picked slice plays
    //     in full at its own length, and finishing IS the cue to pick again.
    //   clock — a fixed outer window (the "clock reference" — e.g. one
    //     quarter note) picks ONE slice + ONE subdivision rate together at
    //     the top of the window, then retriggers that same slice from its
    //     start every subdivision tick for the rest of the window,
    //     regardless of the slice's own natural length (cut short if
    //     longer than the tick, trails into silence if shorter). A new
    //     window always picks fresh.
    enum class TriggerMode { sliceLength, clock };

    void setTriggerMode (TriggerMode mode)
    {
        triggerMode.store (mode);
        clockModeInitialized = false; // force a fresh window/pick on next block
        clockCurrentPickValid = false;
    }

    TriggerMode getTriggerMode() const { return triggerMode.load(); }

    // Fixed palette of note values, shared between the outer clock
    // reference menu and the inner subdivision probability table —
    // expressed in quarter-note ("beat") units so nothing here needs to
    // assume a time signature. Matches the standard Max/M4L tempo-relative
    // rate set (128n up to 1n), capped at one bar as the longest option —
    // 1nd (1.5 bars) is deliberately excluded.
    static constexpr int numNoteValueOptions = 20;
    static juce::String getNoteValueName (int index);
    static double getNoteValueBeats (int index);

    void setClockReferenceIndex (int index)
    {
        clockReferenceIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getClockReferenceIndex() const { return clockReferenceIndex.load(); }

    // Weighted-probability table for which subdivision gets picked each
    // window in Clock mode — same 0-1 weight semantics as slice weights.
    float getSubdivisionProbability (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) subdivisionProbabilities.size())
            return 1.0f;

        return subdivisionProbabilities[(size_t) index];
    }

    void setSubdivisionProbability (int index, float probability)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) subdivisionProbabilities.size())
            subdivisionProbabilities[(size_t) index] = juce::jlimit (0.0f, 1.0f, probability);
    }

    //=== Playback style (Step 19/21/22) ===
    // A weighted table, independent of (but rolled at the same time as)
    // the slice/subdivision picks above: Forward is today's behaviour;
    // Ping-Pong plays a slice forward then immediately backward before
    // the next pick, via the shared foldPosition() mapping in
    // GranularStretcher (used by both pitch modes' render paths, so it
    // behaves identically in Repitch and Time-Stretch); Tape Stop
    // decelerates rate AND gain linearly to zero across a fixed duration
    // (see setTapeStopScope() for how that duration is chosen in Clock
    // mode), as an additional multiplier layered on top of whatever the
    // Pitch Mode already produces — same "shared multiplier" pattern,
    // just applied at the rate/gain level instead of the position level;
    // Stretch always renders through GranularStretcher regardless of the
    // global Pitch Mode setting — a deliberate character effect, not
    // something that should vanish depending on an unrelated toggle —
    // using its own small, hardcoded grain size and a hard-edged window
    // (see stretchCharacterGrainSizeMs/WindowShape::hardEdge), stretching
    // the pick to stretchDurationMultiplier times its natural length.
    // Defaults to Forward-only (weight 0 on everything else) rather than
    // even odds like the other tables — that's what guarantees the
    // default sounds byte-identical to before this existed, not just
    // "usually."
    enum class PlaybackStyle { forward, pingPong, tapeStop, stretch };

    static constexpr int numPlaybackStyleOptions = 4;
    static juce::String getPlaybackStyleName (int index); // "Forward" / "Ping-Pong" / "Tape Stop" / "Stretch"

    float getPlaybackStyleProbability (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) playbackStyleProbabilities.size())
            return 1.0f;

        return playbackStyleProbabilities[(size_t) index];
    }

    void setPlaybackStyleProbability (int index, float probability)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) playbackStyleProbabilities.size())
            playbackStyleProbabilities[(size_t) index] = juce::jlimit (0.0f, 1.0f, probability);
    }

    //=== Tape Stop scope (Step 21) ===
    // Clock-mode-only: how long a Tape Stop pick's decel lasts. Slice
    // Length mode doesn't need this choice — the duration there is always
    // just the pick's own natural slice length, same timebase Forward
    // already uses.
    //   wholeWindow (default) — one continuous decel across the entire
    //     clock reference length, overriding normal subdivision
    //     retriggering for that window (no ticks; the next window picks
    //     fresh as usual).
    //   perTick — each individual subdivision tick gets its own quick
    //     decel-to-zero-and-restart, same cadence Clock mode already
    //     retriggers at — a rapid stutter of small tape-stops rather than
    //     one long sweep.
    enum class TapeStopScope { wholeWindow, perTick };

    static constexpr int numTapeStopScopeOptions = 2;
    static juce::String getTapeStopScopeName (int index); // "Whole window" / "Per tick"

    void setTapeStopScope (TapeStopScope scope) { tapeStopScope.store (scope); }
    TapeStopScope getTapeStopScope() const { return tapeStopScope.load(); }

    //=== Pitch mode (Step 17) ===
    // Independent of Trigger Mode — only changes HOW a pick's audio gets
    // rendered, never when slices get picked/retriggered or how they're
    // weighted. The scheduling logic above (weighted picks, Clock-mode
    // retriggers, fades) is shared unchanged by both:
    //   repitch — today's varispeed behaviour: a single read pointer
    //     advances through the source at playbackRate, so pitch follows
    //     playback speed.
    //   timeStretch — lightweight overlap-add granular synthesis (see
    //     GranularStretcher): short windowed grains play at the source's
    //     native, sample-rate-corrected-only rate (pitch-preserving),
    //     while their START positions get spaced to track tempo, so pitch
    //     stays fixed regardless of speed.
    enum class PitchMode { repitch, timeStretch };

    void setPitchMode (PitchMode mode)
    {
        pitchMode.store (mode);
        granularNeedsReseed.store (true); // reseed the grain engine mid-pick, from wherever playback currently is
    }

    PitchMode getPitchMode() const { return pitchMode.load(); }

    // Grain length for Time-Stretch mode. Overlap is fixed at 50% (not
    // exposed) to keep the UI minimal.
    void setGrainSizeMs (float ms) { grainSizeMs.store (juce::jlimit (20.0f, 150.0f, ms)); }
    float getGrainSizeMs() const { return grainSizeMs.load(); }

    enum class GrainWindowShape { hann, triangular };

    void setGrainWindowShape (GrainWindowShape shape) { grainWindowShape.store (shape); }
    GrainWindowShape getGrainWindowShape() const { return grainWindowShape.load(); }

    // Time-Stretch-only pitch control (Step 18) — a multiplier on each
    // grain's own internal read-rate, entirely separate from the hop
    // scheduling above that controls stretch amount. 0 semitones is a
    // complete no-op (pitchRatio == 1.0), same as before this existed.
    void setPitchShiftSemitones (float semitones) { pitchShiftSemitones.store (juce::jlimit (-24.0f, 24.0f, semitones)); }
    float getPitchShiftSemitones() const { return pitchShiftSemitones.load(); }

    //=== Beat-quantized slice length (Step 24) ===
    // Only takes effect for Pitch Mode == timeStretch AND Trigger Mode ==
    // sliceLength — Clock mode already enforces beat-alignment via its own
    // tick system, so this is simply not consulted there. Default ON
    // whenever Time-Stretch is active: this is the standard behaviour for
    // that mode, not an opt-in extra (unlike every other toggle in this
    // class, which defaults to preserving prior behaviour — Time-Stretch
    // mode itself is still off by default, so nothing changes for anyone
    // who hasn't already opted into it).
    //
    // Per pick (computed once, at pick-start, in the Slice Length while-loop
    // below — see currentPickBeatQuantized/currentPickQuantizedStretchRatio):
    //   1. naturalBeats = (slice length in source seconds) / (60 / originalBpm)
    //      — using the trim/override-aware getCalculatedOriginalBpm() above.
    //      Ping-Pong uses the FULL ROUND TRIP (2x slice length) here, since
    //      that's the unit whose duration should land on the beat grid.
    //   2. Snap naturalBeats to the nearest entry in the existing note-value
    //      palette (getNoteValueBeats()/numNoteValueOptions above — reused
    //      directly, not duplicated) via nearestNoteValueIndex() below.
    //   3. targetHostSeconds = quantizedBeats * (60 / hostBpm)
    //   4. This pick's own stretch ratio = sliceNaturalSourceSeconds /
    //      targetHostSeconds — substituted for the global repitchRatio,
    //      symmetrically, everywhere repitchRatio would otherwise drive
    //      this pick's granular hop schedule AND its scheduling-position
    //      advance rate (see currentPickQuantizedStretchRatio's use in
    //      processBlock). The result: this pick's rendered duration lands
    //      exactly on quantizedBeats, so consecutive picks' durations
    //      always sum to exact beat-grid positions -- drift becomes
    //      structurally impossible rather than something to correct after
    //      the fact.
    // Tape Stop and Stretch skip this entirely (never even computed for
    // those styles) — both already deliberately override natural duration
    // as their whole purpose, and forcing a decel-to-zero or an extreme
    // granular mangle onto an exact beat length would fight the effect
    // rather than serve it.
    //
    // The target-duration calculation itself (steps 1-3 above) is shared
    // with Repitch mode's own separate toggle just below — see
    // computeBeatQuantizeTarget() — since it's identical regardless of
    // pitch mode; only what the resulting ratio gets applied TO differs.
    void setBeatQuantizeSliceLengthEnabled (bool enabled) { beatQuantizeSliceLengthEnabled.store (enabled); }
    bool getBeatQuantizeSliceLengthEnabled() const { return beatQuantizeSliceLengthEnabled.load(); }

    //=== Beat-quantized slice length — Repitch mode (Step 26) ===
    // Same label, same underlying target-duration calculation as the
    // Time-Stretch toggle above (computeBeatQuantizeTarget() is shared, not
    // duplicated) — but its own separate state, since the defaults differ,
    // and its own separate effect: instead of handing the target duration
    // to GranularStretcher's hop schedule, it's used to compute THIS PICK's
    // own varispeed playback rate, the same way repitchRatio already
    // controls duration and pitch together for every other pick. In
    // practice this needs no pitch-mode-specific code at all beyond the
    // pick-start calculation: processBlock()'s shared scheduling-position
    // advance (currentPosition += effectivePlaybackRate) already consults
    // currentPickBeatQuantized/currentPickQuantizedStretchRatio regardless
    // of pitch mode, and in Repitch mode that position IS the direct read
    // pointer — so substituting the quantized ratio there is exactly
    // "adjust the normal repitch-mode rate calculation." This introduces a
    // small per-pick pitch variance, same trade-off already accepted for
    // the Time-Stretch side of this feature — nothing to compensate for or
    // hide.
    //
    // Default OFF, unlike Time-Stretch's default-on: this one has a real
    // pitch trade-off, so it's opt-in rather than the new standard
    // behaviour. With it off (the default), Repitch mode is byte-identical
    // to before this toggle existed.
    //
    // Same exclusions as the Time-Stretch toggle: Tape Stop/Stretch skip
    // it regardless of which Pitch Mode is active, and it only applies in
    // Slice Length trigger mode (Clock mode's tick system already enforces
    // beat-alignment either way).
    void setBeatQuantizeSliceLengthEnabledRepitch (bool enabled) { beatQuantizeSliceLengthEnabledRepitch.store (enabled); }
    bool getBeatQuantizeSliceLengthEnabledRepitch() const { return beatQuantizeSliceLengthEnabledRepitch.load(); }

private:
    // Weighted-random pick across a list of weights. Falls back to
    // uniform-random if every weight is 0 (rather than picking nothing
    // and stalling). Used for both slice selection and, in Clock mode,
    // subdivision selection — same math, different weight lists.
    int pickWeightedIndex (const std::vector<float>& weights)
    {
        if (weights.empty())
            return -1;

        float totalWeight = 0.0f;

        for (auto w : weights)
            totalWeight += juce::jmax (0.0f, w);

        if (totalWeight <= 0.0f)
            return random.nextInt ((int) weights.size());

        const float target = random.nextFloat() * totalWeight;
        float cumulative = 0.0f;

        for (size_t i = 0; i < weights.size(); ++i)
        {
            cumulative += juce::jmax (0.0f, weights[i]);

            if (target <= cumulative)
                return (int) i;
        }

        return (int) weights.size() - 1; // float rounding fallback
    }

    int pickWeightedRandomSlice() { return pickWeightedIndex (sliceProbabilities); }

    // Maps a playbackStyleProbabilities index (as drawn by pickWeightedIndex)
    // to its enum value. A plain out-of-range/negative index (shouldn't
    // happen — the table always has numPlaybackStyleOptions entries) falls
    // back to Forward rather than asserting, matching pickWeightedIndex's
    // own defensive style elsewhere.
    static PlaybackStyle indexToPlaybackStyle (int index)
    {
        if (index == 1) return PlaybackStyle::pingPong;
        if (index == 2) return PlaybackStyle::tapeStop;
        if (index == 3) return PlaybackStyle::stretch;
        return PlaybackStyle::forward;
    }

    // Beat-quantized slice length (Step 24): finds the note-value palette
    // entry (see numNoteValueOptions/getNoteValueBeats() above) closest to
    // targetBeats. Reuses the existing palette directly rather than
    // duplicating it.
    static int nearestNoteValueIndex (double targetBeats);

    // Beat-quantized slice length (Step 24/26) — the shared target-duration
    // calculation both the Time-Stretch and Repitch toggles feed into, so
    // it's computed once here rather than duplicated per pitch mode:
    //   1. naturalBeats = (slice length in source seconds) / (60 / originalBpm)
    //      — Ping-Pong passes pingPong=true, using the FULL ROUND TRIP
    //      (2x sliceLength) as the span whose duration should land on the
    //      beat grid.
    //   2. Snap to the nearest note-value palette entry (nearestNoteValueIndex).
    //   3. targetHostSeconds = quantizedBeats * (60 / hostBpm)
    //   4. stretchRatio = sliceNaturalSourceSeconds / targetHostSeconds —
    //      this pick's own replacement for the global repitchRatio.
    // result.quantized stays false (stretchRatio/targetHostSeconds
    // meaningless) if sliceLength/originalBpm/hostBpm/targetHostSeconds
    // are degenerate (<= 0) — callers check this before using the rest.
    struct BeatQuantizeResult
    {
        bool quantized = false;
        double stretchRatio = 1.0;      // replaces repitchRatio for this pick
        double targetHostSeconds = 0.0; // this pick's target duration, in host seconds
    };

    static BeatQuantizeResult computeBeatQuantizeTarget (int sliceLength, bool pingPong,
                                                          double sampleSampleRate, double originalBpm, double hostBpm);

    // Shared by redetectSlices() and every manual-point mutation: re-runs
    // auto-detection at the given sensitivity, merges the result with the
    // current manual points, sorts + dedupes into one boundary list, and
    // rebuilds `slices` from it. Slice probabilities reset to 1.0 whenever
    // this runs — same known simplification as before Step 10, since
    // slice indices shift around whenever boundaries are added/removed
    // and there's no stable identity to carry a probability value across.
    void rebuildSlicesFromDetectionAndManualPoints (float sensitivity, float holdoffMs);

    // Pure merge logic (no side effects, no member writes) shared by the
    // real rebuild above and previewSlicesAtSensitivity(). Takes a raw
    // auto-detection result (already confined to [trimStart, trimEnd) by
    // the caller) and folds in exclusions + manual points, filtering out
    // any manual point that now falls outside the trim range rather than
    // deleting it outright — same "soft exclude" treatment already used
    // for auto-detected exclusions, so widening the trim again later can
    // bring it back. Must be called with sampleLock already held.
    std::vector<Slice> mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices, int trimStart, int trimEnd) const;

    // Unifies the tempo math (Step 23) that both Trim markers and Manual
    // BPM override feed into:
    //   sourceSpanSeconds = manualBpmOverrideEnabled
    //       ? (loopLengthBars * 4 * 60) / manualBpmOverrideValue
    //       : (trimEndSample - trimStartSample) / sampleSampleRate
    // Used by both getCalculatedOriginalBpm() (the UI's "~X BPM" label) and
    // processBlock()'s repitchRatio — replaces the old calculation, which
    // used the whole buffer's length regardless of trim (the bug this
    // fixes). The existing repitchRatio formula itself (sourceSpanSeconds /
    // hostLoopLengthSeconds) is otherwise unchanged, and GranularStretcher
    // never computes tempo itself — it only ever receives the ratios
    // (repitchRatio, srConversionRatio) this feeds into, so it stays
    // consistent with the direct-read path "for free."
    double computeSourceSpanSeconds() const;

    // Audition (Step 25) — the raw, generative-engine-bypassing loop
    // render. Called from processBlock() (sampleLock already held) in
    // place of everything below it whenever auditionActive is set. Reads/
    // writes auditionPosition; safe from the UI thread too only because
    // setAuditionActive() takes the same lock.
    void renderAudition (juce::AudioBuffer<float>& buffer, double hostSampleRate);

    struct ManualSlicePoint
    {
        int id = -1;
        int samplePosition = 0;
    };

    std::vector<ManualSlicePoint> manualPoints;
    int nextManualPointId = 1;

    struct ExcludedPoint
    {
        int id = -1;
        int samplePosition = 0;
    };

    std::vector<ExcludedPoint> excludedPoints;
    int nextExcludedPointId = 1;

    juce::UndoManager undoManager;

    // Search window for snapping a manual point to the nearest real
    // transient-like peak — generous enough to catch "the hit that's
    // obviously there" without snapping across to an unrelated one.
    static constexpr float manualSnapRadiusMs = 50.0f;

    juce::AudioFormatManager formatManager;

    juce::AudioBuffer<float> sampleBuffer;
    double sampleSampleRate = 44100.0;
    bool sampleLoaded = false;
    juce::String loadedFileName;
    juce::CriticalSection sampleLock; // guards sampleBuffer/slices during loadSample()

    TransientDetector transientDetector;
    std::vector<Slice> slices;
    std::vector<float> sliceProbabilities; // parallel to slices; reset to 1.0 each on redetectSlices()
    juce::Random random;

    std::atomic<int> loopLengthBars { 1 };

    // Trim markers (Step 23) — source-sample-domain bounds confining
    // detection/manual points/playback. Set to the full buffer on load in
    // loadSample(). minTrimGapSamples keeps the two handles from ever
    // crossing/colliding, so the range can never degenerate to zero width.
    std::atomic<int> trimStartSample { 0 };
    std::atomic<int> trimEndSample { 0 };
    static constexpr int minTrimGapSamples = 64;

    // Audition (Step 25) — auditionActive is checked/cleared from the
    // audio thread (auto-stop) and set from the UI thread (button click);
    // auditionPosition is plain (not atomic) since it's only ever touched
    // under sampleLock — by processBlock()/renderAudition() on the audio
    // thread, and by setAuditionActive() on the UI thread.
    std::atomic<bool> auditionActive { false };
    double auditionPosition = 0.0;

    // Manual BPM override (Step 23) — off by default, so behaviour is
    // unchanged (bars-derived tempo, same as always) until the user
    // explicitly enables it. 120 is just a sane inert starting value; it
    // has zero effect while disabled.
    std::atomic<bool> manualBpmOverrideEnabled { false };
    std::atomic<double> manualBpmOverrideValue { 120.0 };

    std::atomic<float> currentSensitivity { defaultSensitivity };
    std::atomic<float> fadeInMs { 5.0f };
    std::atomic<float> fadeOutMs { 15.0f };

    std::atomic<TriggerMode> triggerMode { TriggerMode::sliceLength };
    std::atomic<int> clockReferenceIndex { 13 }; // default: 4n / one quarter note (index in the expanded 20-value table)
    std::vector<float> subdivisionProbabilities; // size numNoteValueOptions, init to 1.0 each
    std::vector<float> playbackStyleProbabilities; // size numPlaybackStyleOptions, init to {1.0, 0.0, 0.0, 0.0} (Forward-only)
    std::atomic<TapeStopScope> tapeStopScope { TapeStopScope::wholeWindow };

    // Stretch (Step 22) character parameters — deliberately fixed, not
    // exposed in the UI (separate from Pitch Mode's user-facing grain
    // size/window shape/pitch shift controls, none of which apply here).
    // Small grains + a hard-edged window make the seams audible; the
    // duration multiplier is what stretches a pick to 4x its natural
    // length regardless of tempo, Pitch Mode, or Pitch Shift.
    static constexpr float stretchCharacterGrainSizeMs = 10.0f; // within the ~8-15ms range asked for
    static constexpr double stretchDurationMultiplier = 4.0;

    // Clock-mode scheduling state (audio thread only). A "window" is one
    // span of the outer clock reference; a "tick" is one subdivision
    // retrigger within that window.
    bool clockModeInitialized = false; // false forces a fresh window on next block
    bool clockCurrentPickValid = false; // false forces a pick even mid-window (very first tick)
    double nextTickPpq = 0.0;
    double windowEndPpq = 0.0;
    int clockCurrentSliceIndex = -1;
    int clockCurrentSubdivisionIndex = -1;
    PlaybackStyle clockCurrentPlaybackStyle = PlaybackStyle::forward; // drawn once per window, alongside the two above

    // Self-chaining playback state — which slice is currently sounding,
    // where we are within it (source sample units), and where it ends.
    // When position reaches the end, the very next sample immediately
    // picks a new slice and continues with zero gap.
    //
    // currentPosition/currentEndSample are the "unfolded" scheduling
    // position — for Ping-Pong, currentEndSample is pushed out to a full
    // round trip (2x slice length) and currentPosition just keeps
    // counting up through it, same as it always has for Forward.
    // currentSliceStartSample/currentSliceLength are the TRUE slice
    // bounds regardless of style, kept separately since currentEndSample
    // no longer is one for Ping-Pong — these feed GranularStretcher::
    // foldPosition() to compute the actual (bounced, for Ping-Pong) read
    // position each render step.
    bool hasCurrentPick = false;
    int currentSliceIndex = -1;
    double currentPosition = 0.0;
    int currentEndSample = 0;
    int currentSliceStartSample = 0;
    int currentSliceLength = 0;
    PlaybackStyle currentPlaybackStyle = PlaybackStyle::forward;

    // Where (in host-output samples since this pick started) a Ping-Pong
    // round trip reverses direction — always one slice's worth of natural
    // (un-doubled) playback time, regardless of how currentPickLength-
    // InHostSamples itself might get shortened by a Clock-mode tick.
    // Meaningless/unused for Forward.
    double currentPickMidpointHostSamples = 0.0;

    // Fixed real-time length (host samples) of a Tape Stop pick's decel
    // ramp — the pick's natural slice length in Slice Length mode; the
    // window or tick length in Clock mode, per Tape Stop scope. Rate and
    // gain both ramp from 1.0 to 0.0 across this, via samplesSincePick-
    // Start / this. Deliberately NOT capped by the slice's own natural
    // length in Clock mode (unlike Forward/Ping-Pong's currentPickLength-
    // InHostSamples) — the whole point is that read position may not
    // reach the slice's actual end before the rate hits zero. Meaningless
    // /unused for Forward/Ping-Pong.
    double currentPickTapeStopDurationHostSamples = 0.0;

    // Lock-free copy of currentSliceIndex, written by the audio thread
    // whenever a new pick begins, read by the UI thread for the playhead
    // highlight. Separate from currentSliceIndex itself so the UI never
    // needs to touch sampleLock just to poll this at 30fps.
    std::atomic<int> currentlyPlayingSliceIndexForUI { -1 };

    // Fade tracking, in host-output-sample units (not source-sample units,
    // so fade length in ms stays constant regardless of repitching).
    // Reset every time a new pick starts.
    double samplesSincePickStart = 0.0;
    double currentPickLengthInHostSamples = 0.0;

    // Pitch mode (Step 17) — Time-Stretch state. granularStretcher is
    // reseeded from currentPosition every time a new pick starts
    // (regardless of which mode is active, so switching mid-pick always
    // finds it already in sync) and again, mid-pick, whenever the mode
    // itself changes (granularNeedsReseed).
    std::atomic<PitchMode> pitchMode { PitchMode::repitch };
    std::atomic<float> grainSizeMs { 60.0f };
    std::atomic<GrainWindowShape> grainWindowShape { GrainWindowShape::hann };
    std::atomic<float> pitchShiftSemitones { 0.0f };
    std::atomic<bool> granularNeedsReseed { false };
    GranularStretcher granularStretcher;

    // Beat-quantized slice length (Step 24) — default ON, see the public
    // setter/getter's doc comment above for why that's correct here
    // (unlike every other toggle in this class).
    std::atomic<bool> beatQuantizeSliceLengthEnabled { true };

    // Beat-quantized slice length — Repitch mode (Step 26). Default OFF,
    // unlike the Time-Stretch toggle above: this one has a real pitch
    // trade-off, so it's opt-in rather than a new standard behaviour.
    std::atomic<bool> beatQuantizeSliceLengthEnabledRepitch { false };

    // Per-pick beat-quantization state (Step 24, audio thread only) —
    // computed once at pick-start in Slice Length mode (never in Clock
    // mode, and never for Tape Stop/Stretch picks — currentPickBeatQuantized
    // stays false for those, and currentPickQuantizedStretchRatio is simply
    // not consulted). Substitutes for repitchRatio, symmetrically, in both
    // this pick's granular hop schedule and its scheduling-position advance
    // rate — see processBlock().
    bool currentPickBeatQuantized = false;
    double currentPickQuantizedStretchRatio = 1.0;

    // Sensible starting defaults — moderate sensitivity, 30ms holdoff to
    // avoid double-triggering on a single drum hit's ringing tail.
    static constexpr float defaultSensitivity = 0.5f;
    static constexpr float defaultHoldoffMs = 30.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessor)
};
