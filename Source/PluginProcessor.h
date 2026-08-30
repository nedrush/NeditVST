#pragma once

#include <JuceHeader.h>
#include "TransientDetector.h"
#include "GranularStretcher.h"
#include "EasingCurve.h"
#include <array>
#include <map>
#include <vector>

//==============================================================================
// STEP 6/8: transport-synced generative playback, plus MIDI-driven pattern
// recall in Sequenced mode (see the MIDI input / Pattern bank section below).
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
//   - MIDI input is otherwise not used to trigger playback (Slice Length and
//     Clock modes remain purely transport-driven) — its only current job is
//     switching between saved Sequencer patterns while Sequenced mode is
//     active, via the dispatch layer described below.
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
    bool acceptsMidi() const override { return true; } // Sequenced mode's pattern-bank recall (see below) needs note-on input
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

    // The loaded sample's own sample rate (not the host's) — needed by
    // WaveformDisplay's zoom (Step 31) to convert a minimum-zoom duration
    // in milliseconds into source samples.
    double getSampleSampleRate() const { return sampleSampleRate; }

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
        redetectSlices (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    float getSensitivity() const { return currentSensitivity.load(); }

    //=== Quantize detected transients to grid (Step 35) ===
    // Auto-detected transients only -- manual points are deliberately
    // user-placed (including via Shift's explicit free-placement bypass),
    // so quantizing them would fight the user's own intent. Snaps each
    // surviving (non-excluded) auto-detected onset to the nearest step on
    // the Grid note-value palette (reusing the existing 20-value palette
    // via getNoteValueName()/getNoteValueBeats() -- no separate table),
    // using the same source-tempo derivation (getCalculatedOriginalBpm(),
    // itself derived from computeSourceSpanSeconds()) already used
    // throughout this class. See quantizeOnsetToGrid() and
    // mergeOnsetsIntoSlices() for exactly where this plugs into the
    // existing detection pipeline. Hard snap only for v1 -- no adjustable
    // blend/strength control, matching the established "ship the simple
    // version first" pattern; add one later only if it turns out to be
    // needed once this has actually been heard in use.
    //
    // Off by default, same "preserve existing behaviour until explicitly
    // opted into" convention as every other toggle in this class. Both
    // setters immediately re-run detection (same as setSensitivityAndRedetect
    // above), since this changes the actual slice boundaries.
    void setQuantizeTransientsEnabled (bool enabled)
    {
        quantizeTransientsEnabled.store (enabled);
        redetectSlices (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    bool getQuantizeTransientsEnabled() const { return quantizeTransientsEnabled.load(); }

    void setQuantizeGridIndex (int index)
    {
        quantizeGridIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
        redetectSlices (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    int getQuantizeGridIndex() const { return quantizeGridIndex.load(); }

    //=== Trim Snap mode (Performance mode's per-state trims only) ===
    // Governs what setTrimStartSample()/setTrimEndSample() snap TO when
    // snapToTransient is true -- Shift still bypasses snapping entirely in
    // either mode, unchanged (see WaveformDisplay::mouseDrag). Transients
    // (default) is the existing behaviour, used everywhere including here
    // unless Grid is explicitly selected. Grid ignores detected transients
    // altogether and instead snaps to a FIXED musical grid, spaced by
    // performanceTrimGridIndex's note value at the sample's already-
    // established tempo (getCalculatedOriginalBpm()) -- see
    // findNearestGridSample() for exactly how. Only consulted while
    // TriggerMode::performance is active; Slice Length/Clock modes' shared
    // global trim always uses Transients, regardless of this setting.
    enum class TrimSnapMode { transients, grid };

    void setPerformanceTrimSnapMode (TrimSnapMode mode) { performanceTrimSnapMode.store (mode); }
    TrimSnapMode getPerformanceTrimSnapMode() const { return performanceTrimSnapMode.load(); }

    // Grid resolution -- same 20-value note-value palette as Clock
    // reference/Quantize Transients' Grid/Subdivide (getNoteValueName()/
    // getNoteValueBeats(), no separate table). Default index 13 (4n / one
    // quarter note), the same default every other note-value-palette
    // control here uses.
    void setPerformanceTrimGridIndex (int index)
    {
        performanceTrimGridIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getPerformanceTrimGridIndex() const { return performanceTrimGridIndex.load(); }

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
    //
    // One exception: the sample's established TEMPO no longer reads these
    // atomics directly -- see tempoTrimStartSample's own comment. Performance
    // mode repoints trimStartSample/trimEndSample at whichever state slot has
    // editing focus (still the right atomics for detection/manual
    // points/"what's playing right now"), while tempoTrimStartSample/
    // tempoTrimEndSample stay pinned to the last REAL trim edit, so per-state
    // playback rate keeps measuring against one stable tempo instead of
    // whichever slot you last clicked.
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
    //
    // Trim Snap mode: while snapToTransient is true AND Performance mode is
    // active AND performanceTrimSnapMode is Grid, findNearestGridSample()
    // replaces the transient search entirely (not layered alongside it) —
    // see its own comment. Outside those conditions (any other trigger
    // mode, or Performance mode still on Transients) behaviour is exactly
    // as before.
    void setTrimStartSample (int sample, bool snapToTransient = true)
    {
        const int currentEnd = trimEndSample.load();
        const int upperBound = juce::jmax (0, currentEnd - minTrimGapSamples); // guards tiny/degenerate buffers
        int target = juce::jlimit (0, upperBound, sample);

        if (snapToTransient)
        {
            target = juce::jlimit (0, upperBound, shouldGridSnapTrim()
                ? findNearestGridSample (target)
                : transientDetector.findNearestPeak (target, (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate)));
        }

        trimStartSample.store (target);

        // Performance mode reuses these same atomics to edit whichever
        // state slot currently has focus (its own, generally much shorter,
        // segment) -- that's never a real change to the sample's
        // established tempo, so the tempo-trim copy sits this one out. See
        // tempoTrimStartSample's own comment for why this distinction
        // exists at all.
        if (triggerMode.load() != TriggerMode::performance)
            tempoTrimStartSample.store (target);

        rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
    }

    void setTrimEndSample (int sample, bool snapToTransient = true)
    {
        const int currentStart = trimStartSample.load();
        const int bufferLength = sampleBuffer.getNumSamples();
        const int lowerBound = juce::jmin (currentStart + minTrimGapSamples, bufferLength); // guards tiny/degenerate buffers
        int target = juce::jlimit (lowerBound, bufferLength, sample);

        if (snapToTransient)
        {
            target = juce::jlimit (lowerBound, bufferLength, shouldGridSnapTrim()
                ? findNearestGridSample (target)
                : transientDetector.findNearestPeak (target, (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate)));
        }

        trimEndSample.store (target);

        // See setTrimStartSample() above -- same reasoning, same guard.
        if (triggerMode.load() != TriggerMode::performance)
            tempoTrimEndSample.store (target);

        rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
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
        {
            auditionPosition = (double) trimStartSample.load(); // always start fresh from the current trim, not wherever a stale position was left
            auditionPlaybackPositionForUI.store (trimStartSample.load()); // immediate UI feedback, rather than waiting for the first rendered block
        }
        else
        {
            auditionPlaybackPositionForUI.store (-1);
        }

        auditionActive.store (active);
    }

    bool getAuditionActive() const { return auditionActive.load(); }

    //=== Audition playhead (Step 28) ===
    // Lock-free copy of the audition engine's current read position, for
    // the waveform's playhead indicator — same pattern as
    // getCurrentlyPlayingSliceIndex() below, just for Audition instead of
    // the generative engine. -1 means "not currently auditioning" (default,
    // and also set whenever audition stops — manually or auto-stopped by
    // host transport starting, per setAuditionActive()/processBlock()).
    // Written every block by renderAudition() while it's running.
    int getAuditionPlaybackPosition() const { return auditionPlaybackPositionForUI.load(); }

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
    void setLoopLengthBars (int bars)
    {
        loopLengthBars.store (juce::jmax (1, bars));

        // Sequenced Trigger Mode (Step 37): column count is derived from
        // loopLengthBars, so any change here invalidates the existing
        // pattern's meaning -- same "reset on rebuild" convention
        // sliceProbabilities already uses.
        const juce::ScopedLock sl (sampleLock);
        resetSequencerGrid();
    }

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

    //=== Trigger mode (Step 14/37) ===
    // Three mutually exclusive ways to decide when the next slice-pick
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
    //   sequenced (Step 37, v1 -- monophonic) — nothing here is randomized;
    //     everything is explicitly placed by the user on a step grid (see
    //     the Sequenced Trigger Mode section below). The entire probability
    //     engine (per-slice weights, playback style table, subdivision
    //     table) sits unused while this mode is active, same as it already
    //     does for whichever OTHER mode's features don't apply to it.
    //   performance (Pass 1) — same "nothing here is randomized" precedent
    //     as sequenced, applied to a single hand-defined segment instead of
    //     a step grid: transient detection and the probability engine are
    //     both unused; the current trim window IS the one segment, and a
    //     128-note-indexed bank of saved (trim, style+params, loop, sync)
    //     states plays back on MIDI recall (see the Performance State Bank
    //     section below). See its own class doc comment on
    //     PerformanceStateSnapshot for the full design.
    //   control — piano-roll slice triggering: ascending chromatically from
    //     a configurable base note (default C1, Simpler's own convention),
    //     one note per detected slice, capped at 32 like the Sequencer's own
    //     row cap; a fixed block of keyswitch notes immediately below the
    //     base note (one per PlaybackStyle, keyswitchNote = baseNote - 1 -
    //     styleIndex — see getControlKeyswitchNote() below) sets which style
    //     plays for every slice note-on from that point forward. No
    //     assignable storage at all -- the mapping is pure arithmetic, so
    //     moving the base note shifts the whole keyswitch block with it for
    //     free. No probability engine, no window/tick concept — same
    //     "nothing here is randomized," transport-independent precedent as
    //     Performance mode, just driven by whichever slice note-on arrives
    //     rather than a saved snapshot.
    enum class TriggerMode { sliceLength, clock, sequenced, performance, control };

    void setTriggerMode (TriggerMode mode)
    {
        triggerMode.store (mode);
        clockModeInitialized = false; // force a fresh window/pick on next block
        clockCurrentPickValid = false;
        resetWindowInitialized = false; // Step 34 -- same "start fresh, aligned" guarantee entering Slice Length mode
        sequencedModeInitialized = false; // Step 37 -- same guarantee entering Sequenced mode
        performanceModeInitialized = false; // Pass 1 -- same guarantee entering Performance mode
        controlModeInitialized = false; // same guarantee entering Control mode

        // Leaving Control mode abandons a note-on pick still waiting to be
        // consumed by the per-sample loop, and any Gate release fade
        // already in progress -- same "leaving the mode abandons transient
        // state" reasoning as MIDI Learn just below, applied to Control
        // mode's own audio-thread-only pending state.
        if (mode != TriggerMode::control)
        {
            controlNoteOnPending = false;
            controlGateReleaseActive = false;
        }

        // Pattern bank MIDI Learn only makes sense while Sequenced mode is
        // selected (it's the only mode whose UI can arm it) -- leaving the
        // mode abandons any learn still armed, so it can never later capture
        // a stale snapshot into the wrong slot after the user's moved on.
        // Same reasoning for a pattern switch left pending (Pass 2) -- its
        // boundary check only ever runs in the sequencedMode branch below,
        // so leaving the mode would otherwise strand it, silently applying
        // whenever Sequenced mode is re-entered later.
        if (mode != TriggerMode::sequenced)
        {
            midiLearnArmed.store (false);
            pendingPatternSwitchNote.store (-1);
        }

        // Performance mode's own editing focus (focusedPerformanceStateSlot)
        // deliberately persists across a mode change -- unlike Sequenced
        // mode's MIDI Learn above, there's nothing here that would go stale
        // by staying armed, since focus isn't a transient "waiting for the
        // next note-on" state; it just remembers which slot to resume
        // editing next time Performance mode is selected again.
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

    //=== Playback style (Step 19/21/22/29) ===
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
    // using its own adjustable grain size and a hard-edged window (see
    // setStretchGrainSizeMs()/WindowShape::hardEdge), stretching the pick
    // to setStretchSpeedMultiplier() times its natural length (Step 46 --
    // both per-step-overridable in Sequenced mode, see below);
    // Filter Down/Filter Up (Step 29/30) both apply the same resonant
    // filter (see filterSweepFilter, filter type overridable per Step 46)
    // as post-processing on this pick's
    // rendered output, cutoff swept log-scale across the pick's duration —
    // Down sweeps ~9kHz -> ~250Hz (open to closed, the classic breakbeat/
    // DnB "filter close"), Up is the mirror image, ~250Hz -> ~9kHz. Scope
    // (see FilterSweepScope below) picks what "across the pick's duration"
    // means in Clock mode. "Works on the output regardless of how it was
    // generated" is what makes these the only styles needing zero
    // scheduling special-casing (no currentEndSample/currentPickLength-
    // InHostSamples override, no beat-quantize exclusion code, no forced
    // Clock-mode retrigger override — they already fall through to
    // exactly the same paths Forward does for all of that, and just get
    // an extra post-process step layered on top).
    // Defaults to Forward-only (weight 0 on everything else) rather than
    // even odds like the other tables — that's what guarantees the
    // default sounds byte-identical to before this existed, not just
    // "usually."
    // Bitcrush (Step 48) extends the same enum/table with a seventh entry
    // -- a pure post-processing pass (sample-and-hold rate reduction +
    // bit-depth quantization, see processBlock()'s bitcrushActive branch)
    // layered onto the pick's rendered output exactly the same way Filter
    // Down/Up's sweep is, which is what makes it need zero scheduling
    // special-casing either -- it isn't tapeStop/stretch/pingPong, so it
    // already falls through to every duration/looping/beat-quantize branch
    // above the same way Filter Down/Up does (see those branches' own
    // comments). Slice Length/Clock modes (and any Sequenced step without
    // its own override) always render it Static at the fixed default
    // (bitcrushRateReductionDefault/bitcrushBitDepthDefault in
    // PluginProcessor.cpp, matching the original fixed-constant values) --
    // Sample Rate Reduction and Bit Depth (Step 49) are per-step
    // adjustable, each with an independent Static/Sweep In/Sweep Out mode,
    // via the same right-click menu mechanism as Resonance/Grain Size --
    // see getApplicableSequencerCellParameters() and
    // SequencerGrid::showParameterMenuForCell()'s swept-parameter branch.
    // Scratch (v1) extends the table with an eighth entry -- a separate
    // style from Ping-Pong (its own probability weight/right-click
    // parameters) that reuses the exact same GranularStretcher::
    // foldPosition() bounce mechanism Ping-Pong already drives, just at a
    // much faster, adjustable rate: its own Rate parameter (index 10,
    // right-click only -- see getApplicableSequencerCellParameters()) picks
    // a note-value duration for ONE forward-backward cycle (default 16n)
    // from the shared note-value palette, the same "palette-style picker"
    // pattern Clock reference/Subdivide already use, not a continuous
    // slider. Step-extension loops additional Rate-length cycles to fill
    // an extended step -- identical mechanism to Ping-Pong's own
    // extension (see the pingPongActive/scratchActive-shared "bounce" fold
    // length in processBlock()), just with the Rate cycle as the repeating
    // unit instead of a single round trip through the slice. Slice Length/
    // Clock modes (and any Sequenced step without its own override)
    // always use the fixed default Rate (scratchDefaultRateIndex in
    // PluginProcessor.cpp, same "no global dial" precedent as Subdivide/
    // Bitcrush). v2 adds independent Forward Curve/Backward Curve
    // parameters (indices 11/12) -- each stroke's actual playback RATE
    // (not just where foldPosition() reflects it) now follows the chosen
    // EasingCurve shape across that stroke, rather than v1's constant
    // speed -- see EasingCurve.h and GranularStretcher::foldPosition()'s
    // own forwardCurve/backwardCurve params for the mechanism, and the
    // scratchActive branch in processBlock() for how the two per-step
    // curve choices get captured and fed into it. Both default to Linear
    // (index 0) when no override exists -- v1's exact constant-speed
    // sound -- same "no global dial" precedent as Rate itself.
    // Flanger extends the table with a ninth entry, following the exact
    // same shape as Bitcrush -- a post-processing pass (a short delay
    // line mixed with the dry signal, with its own adjustable Feedback
    // feeding the delayed signal back into the line for a genuinely
    // resonant comb character, see processBlock()'s flangerActive branch)
    // layered onto the pick's rendered output in the SAME slot Bitcrush/
    // Filter Sweep already use, which is why it needs zero scheduling
    // special-casing either -- it isn't tapeStop/stretch/pingPong/scratch,
    // so it already falls through to every duration/looping/beat-quantize
    // branch above exactly the way Bitcrush does. Slice Length/Clock modes
    // (and any Sequenced step without its own override) always render it
    // Static at the fixed default (flangerDelayTimeDefaultMs/
    // flangerMixDefault/flangerFeedbackDefault in PluginProcessor.cpp) --
    // Delay Time, Mix, and Feedback are all per-step adjustable, each with
    // an independent Static/Sweep In/Sweep Out mode, via the same
    // right-click menu mechanism (and the same shared sweep-mode option
    // list) as Sample Rate Reduction/Bit Depth -- see
    // getApplicableSequencerCellParameters() and SequencerGrid::
    // showParameterMenuForCell()'s swept-parameter branch. Feedback is
    // clamped to a fixed extreme well short of 100% (flangerFeedbackExtreme,
    // 0.88 -- see getSequencerCellParameterMax()) so Sweep In/Static-maxed
    // reaches a pronounced, genuinely resonant character without ever
    // being able to self-oscillate/runaway. Unlike
    // Bitcrush's Sweep In/Out (measured against the individual pick's own
    // duration), Flanger's sweep is measured against Filter Sweep's Whole
    // Window progress (samplesSinceWindowStart/currentWindowLengthHost-
    // Samples) wherever a window exists -- see flangerUseWholeWindow in
    // processBlock() -- so it glides once across an entire Clock-mode
    // window or Sequenced-mode step while Subdivide retriggers happen
    // underneath, rather than resetting on every retrigger.
    enum class PlaybackStyle { forward, pingPong, tapeStop, stretch, filterSweepDown, filterSweepUp, bitcrush, scratch, flanger };

    static constexpr int numPlaybackStyleOptions = 9;
    static juce::String getPlaybackStyleName (int index); // "Forward" / "Ping-Pong" / "Tape Stop" / "Stretch" / "Filter Down" / "Filter Up" / "Bitcrush" / "Scratch" / "Flanger"

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

    // Per-style "randomize parameters" opt-in (Sequenced mode's Randomize
    // Sequence button), one flag per PlaybackStyle, all defaulting to false
    // -- unchecked reproduces today's Randomize behaviour exactly (slice +
    // style only, no per-step overrides). When randomizeSequence() places a
    // step whose style has this set, it also rolls a random value (and,
    // for swept parameters, an independent random Static/Sweep In/Sweep
    // Out mode) for every parameter getApplicableSequencerCellParameters()
    // lists for that style, MINUS Subdivide (5) and Volume (19) -- both are
    // general/style-independent, not "this style's own" parameters, same
    // exclusion PlaybackStyleParameterPanel's buildColumnsForStyle() already
    // applies. Styles with nothing left after that exclusion (Forward, the
    // only one currently) are unaffected regardless of this flag, since
    // there's nothing to randomize. Purely a UI toggle -- manually-drawn
    // steps never consult this, only randomizeSequence() does.
    bool getRandomizeParametersForStyle (int index) const
    {
        const juce::ScopedLock sl (sampleLock);

        if (index < 0 || index >= (int) randomizeParametersForStyle.size())
            return false;

        return randomizeParametersForStyle[(size_t) index];
    }

    void setRandomizeParametersForStyle (int index, bool shouldRandomize)
    {
        const juce::ScopedLock sl (sampleLock);

        if (index >= 0 && index < (int) randomizeParametersForStyle.size())
            randomizeParametersForStyle[(size_t) index] = shouldRandomize;
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

    //=== Slice Length periodic reset (Step 34) ===
    // Mandatory (no "Off" option) -- Slice Length mode has always been
    // purely self-paced by natural pick completion, with zero host-
    // position awareness, which is exactly why it's been able to drift
    // arbitrarily far from the beat grid over a long session (Clock mode
    // never has this problem, since its own window/tick system already
    // keeps it host-position-locked). This forces a hard resync every
    // resetBars bars: whatever's currently playing gets cut off (cleanly
    // faded, never clicked -- see processBlock()) exactly on the boundary,
    // and a fresh weighted pick starts right there. See processBlock()'s
    // resetWindowEndPpq tracking for the mechanism -- a lightweight
    // version of Clock mode's own per-sample window-boundary detection,
    // reused directly rather than re-derived.
    // Visible only in Slice Length mode -- Clock mode already has its own
    // window-boundary mechanism via clockReferenceIndex and doesn't need
    // this at all.
    static constexpr int numResetBarsOptions = 4;
    static juce::String getResetBarsName (int index); // "1 bar" / "2 bars" / "4 bars" / "8 bars"
    static int getResetBarsValue (int index);         // 1 / 2 / 4 / 8

    void setResetBarsIndex (int index) { resetBarsIndex.store (juce::jlimit (0, numResetBarsOptions - 1, index)); }
    int getResetBarsIndex() const { return resetBarsIndex.load(); }

    //=== Filter Sweep scope (Step 30) ===
    // Clock-mode-only, same visibility pattern as Tape Stop scope above —
    // but its own separate state (defaults differ) and a different
    // relationship to ticks:
    //   perTick (default) — today's behaviour, unchanged: sweep progress
    //     is samplesSincePickStart / currentPickLengthInHostSamples,
    //     resetting at every individual retrigger, same as Slice Length
    //     mode always uses regardless of this setting.
    //   wholeWindow — ticks keep retriggering normally at the subdivision
    //     rate (NOT overridden the way Tape Stop's wholeWindow overrides
    //     normal retriggering) — only the cutoff's progress fraction
    //     changes, to samplesSinceWindowStart / currentWindowLengthHost-
    //     Samples, continuous across every tick in that window and only
    //     reset when a new window begins.
    // Default is perTick, the OPPOSITE of Tape Stop scope's wholeWindow
    // default — Filter Sweep's existing behaviour (from Step 29, before
    // this scope choice existed) was already per-pick/per-tick, so this
    // default is what keeps that behaviour unchanged for anyone who's
    // already using it.
    enum class FilterSweepScope { wholeWindow, perTick };

    static constexpr int numFilterSweepScopeOptions = 2;
    static juce::String getFilterSweepScopeName (int index); // "Whole window" / "Per tick"

    void setFilterSweepScope (FilterSweepScope scope) { filterSweepScope.store (scope); }
    FilterSweepScope getFilterSweepScope() const { return filterSweepScope.load(); }

    //=== Filter Sweep resonance (Step 45) ===
    // Was a compile-time constant (filterSweepResonance = 2.0) until now
    // -- turned into a stored, adjustable value so Sequenced mode's
    // per-step overrides (see the sequencer step parameter section below)
    // have something to override, and so a future global slider for
    // Slice Length/Clock modes can read/write the same value. Default
    // matches the original hardcoded constant exactly, so nothing changes
    // for existing behavior until either mechanism actually touches it.
    static constexpr float defaultFilterSweepResonance = 2.0f;
    static constexpr float minFilterSweepResonance = 0.5f;
    static constexpr float maxFilterSweepResonance = 10.0f;

    void setFilterSweepResonance (float resonance)
    {
        filterSweepResonanceValue.store (juce::jlimit (minFilterSweepResonance, maxFilterSweepResonance, resonance));
    }

    float getFilterSweepResonance() const { return filterSweepResonanceValue.load(); }

    //=== Filter Sweep filter type (Step 46) ===
    // Extends Filter Sweep resonance's per-step-override mechanism (see
    // sequencer step parameter overrides below) to a second Filter Down/
    // Up-only parameter: which filter type the shared filterSweepFilter
    // renders through, not just its resonance. Index-based (0/1/2), same
    // convention as resetBarsIndex/stepResolutionIndex below rather than
    // a dedicated enum, since it's stored the same generic way sequencer
    // step overrides store every other parameter (a plain float, indexed
    // by name). Default (0 -- lowpass) matches the filter's original
    // hardcoded setType() call exactly, so nothing changes for existing
    // users until this or a per-step override actually touches it.
    static constexpr int numFilterSweepFilterTypeOptions = 3;
    static juce::String getFilterSweepFilterTypeName (int index); // "Low-pass" / "High-pass" / "Band-pass"

    void setFilterSweepFilterType (int index) { filterSweepFilterTypeValue.store (juce::jlimit (0, numFilterSweepFilterTypeOptions - 1, index)); }
    int getFilterSweepFilterType() const { return filterSweepFilterTypeValue.load(); }

    //=== Curve shape (Step 46) ===
    // A per-step-override-capable parameter shared between Tape Stop's
    // decel and Ping-Pong's turnaround fade (see processBlock) -- both
    // already computed a 0..1 progress fraction driving a linear ramp, so
    // "Exponential" is a drop-in substitute for that fraction (see the
    // applyCurveShape() helper in PluginProcessor.cpp) rather than a
    // separate code path per style. Default (0 -- Linear) matches both
    // styles' existing behaviour exactly.
    static constexpr int numCurveShapeOptions = 2;
    static juce::String getCurveShapeName (int index); // "Linear" / "Exponential"

    void setCurveShape (int index) { curveShapeValue.store (juce::jlimit (0, numCurveShapeOptions - 1, index)); }
    int getCurveShape() const { return curveShapeValue.load(); }

    //=== Stretch grain settings (Step 46) ===
    // Stretch playback style's own grain size/speed -- separate from
    // Pitch Mode Time-Stretch's grainSizeMs/pitchShiftSemitones above,
    // neither of which apply to this style (see processBlock's
    // stretchActive branch). Turns the two values that branch used to
    // hardcode (stretchCharacterGrainSizeMs/stretchDurationMultiplier,
    // now stretchGrainSizeMsValue/stretchSpeedMultiplierValue below) into
    // stored, adjustable values with per-step overrides -- same "was a
    // compile-time constant, now overridable" treatment Filter Sweep
    // resonance got in Step 45. Defaults match the original constants
    // exactly, so nothing changes until either mechanism touches them.
    static constexpr float defaultStretchGrainSizeMs = 10.0f; // within the ~8-15ms range originally hardcoded
    static constexpr float minStretchGrainSizeMs = 5.0f;
    static constexpr float maxStretchGrainSizeMs = 30.0f;

    void setStretchGrainSizeMs (float ms) { stretchGrainSizeMsValue.store (juce::jlimit (minStretchGrainSizeMs, maxStretchGrainSizeMs, ms)); }
    float getStretchGrainSizeMs() const { return stretchGrainSizeMsValue.load(); }

    // "Speed" here is a FIXED character constant -- how many times slower
    // than normal playback grains march through the source material for
    // ONE pass (originally the fixed stretchDurationMultiplier = 4.0).
    // Higher values feel more stretched/slower within that one pass;
    // lower values stay closer to natural pace. Deliberately independent
    // of how long a pick actually plays (Step-extension fix) -- that's
    // authoritative from the pick's own declared length instead (see
    // processBlock()'s stretchActive branches); if the declared length
    // outlasts one pass, the SAME pass just repeats (GranularStretcher::
    // PlaybackStyle::loop) to fill the remainder, rather than this value
    // being stretched further to fit.
    static constexpr float defaultStretchSpeedMultiplier = 4.0f;
    static constexpr float minStretchSpeedMultiplier = 1.0f;
    static constexpr float maxStretchSpeedMultiplier = 8.0f;

    void setStretchSpeedMultiplier (float multiplier) { stretchSpeedMultiplierValue.store (juce::jlimit (minStretchSpeedMultiplier, maxStretchSpeedMultiplier, multiplier)); }
    float getStretchSpeedMultiplier() const { return stretchSpeedMultiplierValue.load(); }

    //=== Bitcrush/Flanger/Scratch global defaults (Slice Length/Clock mode
    // parameter panel) ===
    // These nine parameters (Bitcrush's Sample Rate Reduction/Bit Depth,
    // each with a Static/Sweep In/Sweep Out Mode; Flanger's Delay Time/
    // Mix/Feedback, each with its own Mode; Scratch's Rate/Forward Curve/
    // Backward Curve) had no adjustable global dial before now -- Slice
    // Length/Clock mode picks always used a fixed fallback constant (see
    // bitcrushRateReductionDefault etc. in PluginProcessor.cpp), the same
    // "no global dial" precedent Subdivide still follows. Turned into
    // stored, adjustable values here, same "was a compile-time constant,
    // now overridable" treatment Filter Sweep resonance/Stretch grain
    // settings got above -- defaults match the original fixed constants
    // exactly, so nothing changes until the parameter panel actually
    // touches one. Min/max/option-count clamping reuses the generic
    // getSequencerCellParameterMin()/Max()/NumOptions() rather than
    // duplicating per-parameter range constants here (indices 6-18 match
    // getSequencerCellParameterName()'s own ordering).
    void setBitcrushRateReductionGlobal (float value) { bitcrushRateReductionGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (6), getSequencerCellParameterMax (6), value)); }
    float getBitcrushRateReductionGlobal() const { return bitcrushRateReductionGlobalValue.load(); }
    void setBitcrushRateReductionModeGlobal (int mode) { bitcrushRateReductionModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (7) - 1, mode)); }
    int getBitcrushRateReductionModeGlobal() const { return bitcrushRateReductionModeGlobalValue.load(); }

    void setBitcrushBitDepthGlobal (float value) { bitcrushBitDepthGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (8), getSequencerCellParameterMax (8), value)); }
    float getBitcrushBitDepthGlobal() const { return bitcrushBitDepthGlobalValue.load(); }
    void setBitcrushBitDepthModeGlobal (int mode) { bitcrushBitDepthModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (9) - 1, mode)); }
    int getBitcrushBitDepthModeGlobal() const { return bitcrushBitDepthModeGlobalValue.load(); }

    void setScratchRateGlobal (int index) { scratchRateGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (10) - 1, index)); }
    int getScratchRateGlobal() const { return scratchRateGlobalValue.load(); }
    void setScratchForwardCurveGlobal (int index) { scratchForwardCurveGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (11) - 1, index)); }
    int getScratchForwardCurveGlobal() const { return scratchForwardCurveGlobalValue.load(); }
    void setScratchBackwardCurveGlobal (int index) { scratchBackwardCurveGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (12) - 1, index)); }
    int getScratchBackwardCurveGlobal() const { return scratchBackwardCurveGlobalValue.load(); }

    void setFlangerDelayTimeGlobal (float value) { flangerDelayTimeGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (13), getSequencerCellParameterMax (13), value)); }
    float getFlangerDelayTimeGlobal() const { return flangerDelayTimeGlobalValue.load(); }
    void setFlangerDelayTimeModeGlobal (int mode) { flangerDelayTimeModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (14) - 1, mode)); }
    int getFlangerDelayTimeModeGlobal() const { return flangerDelayTimeModeGlobalValue.load(); }

    void setFlangerMixGlobal (float value) { flangerMixGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (15), getSequencerCellParameterMax (15), value)); }
    float getFlangerMixGlobal() const { return flangerMixGlobalValue.load(); }
    void setFlangerMixModeGlobal (int mode) { flangerMixModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (16) - 1, mode)); }
    int getFlangerMixModeGlobal() const { return flangerMixModeGlobalValue.load(); }

    void setFlangerFeedbackGlobal (float value) { flangerFeedbackGlobalValue.store (juce::jlimit (getSequencerCellParameterMin (17), getSequencerCellParameterMax (17), value)); }
    float getFlangerFeedbackGlobal() const { return flangerFeedbackGlobalValue.load(); }
    void setFlangerFeedbackModeGlobal (int mode) { flangerFeedbackModeGlobalValue.store (juce::jlimit (0, getSequencerCellParameterNumOptions (18) - 1, mode)); }
    int getFlangerFeedbackModeGlobal() const { return flangerFeedbackModeGlobalValue.load(); }

    //=== Delay/Reverb send buses (Pass 1) ===
    // Two persistent effect buses, entirely independent of any pick's
    // lifecycle -- unlike every parameter above, which is per-pick state
    // reset at each new pick's start, these keep processing continuously
    // block after block regardless of whether a pick is currently
    // rendering (see processSendBuses() in PluginProcessor.cpp). Outside
    // Sequenced mode, each pick's output is still SENT to a bus at a fixed
    // 50% (no per-mode control exists for those); Sequenced mode instead
    // uses each firing step's own per-cell Send Amount override (Pass 2,
    // see getSequencerCellParameterOverride() calls in the sequencedMode
    // branch of processBlock(), and SequencerGrid's "Send to Delay"/"Send
    // to Reverb" right-click submenus) -- opt-in, 0% (no send) for any step
    // that's never had it set. Return Level is how loud that bus's own
    // processed signal is mixed back into the main output -- a send/return
    // model, not a per-source insert mix.
    //
    // Time/Feedback (Delay) and Size/Decay (Reverb) below are this ONE
    // shared bus's own live character -- these setters (the panel's own
    // dials) apply instantly, same as always, but also re-arm the matching
    // ramp TARGET (delayBusTimeMsRampTargetValue etc., see processSendBuses())
    // to the same value, so a manual dial turn is never immediately fought
    // by a stale pending ramp left over from an earlier Sequenced-mode
    // step's character override. A firing step's own Time/Feedback/Size/
    // Decay override (Pass 2) instead only moves the RAMP TARGET (see
    // processBlock()'s sequencedMode branch) -- processSendBuses() eases
    // the live value toward it over a short, fixed duration every block,
    // audio-thread-only, so the bus's character glides rather than jumps
    // (an instant jump mid-decay/mid-tail would click, since this is a
    // continuously-running processor, not a per-note instance -- see this
    // feature's own top-level doc comment). A step with no override for a
    // given parameter never touches that parameter's ramp target at all,
    // so it just stays wherever it last settled.
    void setDelayBusTimeMs (float value)
    {
        const float clamped = juce::jlimit (1.0f, 2000.0f, value);
        delayBusTimeMsValue.store (clamped);
        delayBusTimeMsRampTargetValue.store (clamped);
    }
    float getDelayBusTimeMs() const { return delayBusTimeMsValue.load(); }
    void setDelayBusFeedback (float value)
    {
        const float clamped = juce::jlimit (0.0f, 0.95f, value);
        delayBusFeedbackValue.store (clamped);
        delayBusFeedbackRampTargetValue.store (clamped);
    }
    float getDelayBusFeedback() const { return delayBusFeedbackValue.load(); }
    void setDelayBusReturnLevel (float value) { delayBusReturnLevelValue.store (juce::jlimit (0.0f, 1.0f, value)); }
    float getDelayBusReturnLevel() const { return delayBusReturnLevelValue.load(); }

    // Size maps directly onto juce::dsp::Reverb's own roomSize; Decay maps
    // onto its damping, inverted (a higher Decay value means less
    // high-frequency damping, i.e. a longer-sounding tail) -- see
    // processSendBuses() for where these are actually applied.
    void setReverbBusSize (float value)
    {
        const float clamped = juce::jlimit (0.0f, 1.0f, value);
        reverbBusSizeValue.store (clamped);
        reverbBusSizeRampTargetValue.store (clamped);
    }
    float getReverbBusSize() const { return reverbBusSizeValue.load(); }
    void setReverbBusDecay (float value)
    {
        const float clamped = juce::jlimit (0.0f, 1.0f, value);
        reverbBusDecayValue.store (clamped);
        reverbBusDecayRampTargetValue.store (clamped);
    }
    float getReverbBusDecay() const { return reverbBusDecayValue.load(); }
    void setReverbBusReturnLevel (float value) { reverbBusReturnLevelValue.store (juce::jlimit (0.0f, 1.0f, value)); }
    float getReverbBusReturnLevel() const { return reverbBusReturnLevelValue.load(); }

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

    //=== Sequenced Trigger Mode (Step 37, v1 -- monophonic) ===
    // A mouse-drawable step grid: rows are available slices, columns are
    // steps. Structural monophony is enforced at the INPUT level (see
    // setSequencerCell() below), not just at playback -- only one cell may
    // be active per column across the whole grid, so activating a cell in
    // one row automatically clears any active cell elsewhere in that same
    // column. That's what keeps "the whole sequencer shares one voice"
    // true and unambiguous from the moment a pattern is being drawn, not
    // just something the playback engine happens to guarantee afterward,
    // and it's also what avoids needing a tie-break rule entirely.
    //
    // Grid dimensions:
    //   rows -- one per available slice (auto-detected + manual, pooled
    //     from the existing `slices` list, same source everything else
    //     already reads), capped at numSequencerRows (32). If more than 32
    //     slices exist, only the first 32 in time-order are representable
    //     -- a known v1 limitation, not solved here.
    //   columns ("steps") -- patternLengthBars * 4 * stepsPerBeat, where
    //     stepsPerBeat comes from the Step resolution dropdown (reusing
    //     the existing note-value palette directly -- e.g. selecting 16th
    //     notes gives 4 steps per beat; 2 bars at 16th notes = 32 steps).
    //     patternLengthBars (Step 38) is its own dedicated control, NOT
    //     loopLengthBars -- loopLengthBars governs the loaded audio's
    //     tempo calculation and has no reason to match how many bars the
    //     drawn pattern itself spans; conflating the two was what caused
    //     Sequenced mode to only ever offer 1 bar's worth of steps before
    //     this existed.
    //
    // The pattern is reset to all-off whenever any dimension changes
    // (slices rebuild, pattern length changes, or step resolution changes)
    // -- there's no way to meaningfully preserve a 2D pattern across a
    // dimension change, and this matches the same "reset to a sane
    // default whenever the underlying structure changes" convention
    // sliceProbabilities already uses on every redetection.
    //
    // See processBlock() for the playback side: it reuses the exact same
    // currentPosition/currentEndSample/hasCurrentPick single-voice render
    // path every other mode already uses -- the only new logic is
    // scheduling (track host ppq, same pattern as Clock mode; determine
    // the current step; when a NEW active step is reached, immediately
    // set currentPosition/currentEndSample to that row's slice, same
    // "force a fresh start regardless of what's currently playing"
    // mechanic already proven in Clock mode's tick-retriggering and the
    // mandatory Reset feature). Each step fires as whichever PlaybackStyle
    // its own cell stores (Step 41) -- the exact same Ping-Pong fold/Tape
    // Stop decel/forced-granular Stretch/Filter Sweep render code Slice
    // Length and Clock modes already use, just selected directly from the
    // cell instead of via a weighted draw. Tape Stop and Filter Sweep
    // always behave as "Per Tick" here (there's no "Whole Window" concept
    // in Sequenced mode) -- this needs no extra code since both scope
    // settings are already gated to clockMode elsewhere in processBlock().
    // Polyphony and more than 32 rows remain deferred past v1.
    static constexpr int numSequencerRows = 32;

    // Defensive cap purely for UI/performance sanity at extreme parameter
    // combinations (e.g. 8 bars at 128th notes would otherwise be 1024
    // columns) -- same "known v1 limitation" spirit as the row cap above,
    // just applied symmetrically to columns.
    static constexpr int maxSequencerColumns = 256;

    int getSequencerNumRows() const { return juce::jmin (numSequencerRows, (int) slices.size()); }

    int getSequencerNumSteps() const
    {
        const double gridBeats = getNoteValueBeats (stepResolutionIndex.load());
        const double stepsPerBeat = (gridBeats > 0.0) ? (1.0 / gridBeats) : 1.0;
        const int patternBars = getPatternLengthBarsValue (patternLengthBarsIndex.load());
        const int rawSteps = juce::roundToInt ((double) patternBars * 4.0 * stepsPerBeat);
        return juce::jlimit (1, maxSequencerColumns, rawSteps);
    }

    // Pattern length (Step 38) -- 1/2/4 bars, deliberately separate from
    // loopLengthBars (see the class-level doc comment above). Changing it
    // changes the column count, so it resets the grid the same way Step
    // resolution already does.
    static constexpr int numPatternLengthBarsOptions = 3;
    static juce::String getPatternLengthBarsName (int index); // "1 bar" / "2 bars" / "4 bars"
    static int getPatternLengthBarsValue (int index);         // 1 / 2 / 4

    void setPatternLengthBarsIndex (int index)
    {
        patternLengthBarsIndex.store (juce::jlimit (0, numPatternLengthBarsOptions - 1, index));
        const juce::ScopedLock sl (sampleLock);
        resetSequencerGrid(); // column count just changed
    }

    int getPatternLengthBarsIndex() const { return patternLengthBarsIndex.load(); }

    // Step resolution -- reuses the same 20-value note-value palette as
    // Clock reference/Quantize Transients' Grid, rather than a separate
    // table. Defaults to index 7 (16n / a sixteenth note), matching the
    // spec's own worked example (16th notes -> 4 steps per beat).
    void setStepResolutionIndex (int index)
    {
        stepResolutionIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
        const juce::ScopedLock sl (sampleLock);
        resetSequencerGrid(); // column count just changed
    }

    int getStepResolutionIndex() const { return stepResolutionIndex.load(); }

    // Cell state (Step 41): each cell stores -1 (empty) or a
    // PlaybackStyle index (0 to numPlaybackStyleOptions-1, same ordinal as
    // the enum/indexToPlaybackStyle() below and playbackStyleProbabilities'
    // ordering) -- reusing the existing PlaybackStyle enum rather than a
    // parallel one. row/column outside the current grid dimensions are
    // silently ignored (defensive -- the UI should never ask for an
    // out-of-range cell, but dimensions can shift between a mouse event
    // being queued and processed).
    int getSequencerCellStyle (int row, int column) const; // -1 if empty or out-of-range
    void setSequencerCell (int row, int column, int style); // style -1 clears; 0 to numPlaybackStyleOptions-1 sets that PlaybackStyle

    // Step-extension (Pass 1, mechanism only) -- an optional per-cell
    // "extended length in steps" override, on top of the style set by
    // setSequencerCell() above. Unset (0) means "use today's behaviour" --
    // the step's natural slice length, exactly as if this feature didn't
    // exist. Set via Shift+drag from an active step's own right edge in
    // SequencerGrid (see its class doc comment); growing into columns
    // another row already occupies clears those conflicting cells, the
    // exact same per-column monophony rule setSequencerCell() already
    // enforces for a plain single-cell draw (see its own implementation
    // just below) -- just applied across the whole newly-claimed span
    // instead of one column. Only an already-active cell can be extended;
    // a no-op on an empty one. Cleared automatically whenever this cell's
    // own style changes (including cleared) via setSequencerCell(), or the
    // grid resets/wipes -- same lifecycle as the parameter-override map
    // above.
    int getSequencerCellExtendedLengthSteps (int row, int column) const; // 0 if unset/empty/out-of-range
    void setSequencerCellExtendedLengthSteps (int row, int column, int lengthSteps); // clamped into [1, numSteps - column]; no-op on an empty cell

    // `row`'s slice, expressed in steps at the current Step-resolution --
    // i.e. its natural playback length quantized to the sequencer grid,
    // the same calculation SequencerGrid's piano-roll bar has always
    // started from. Shared here (not just computed in the UI) so the audio
    // thread can read the exact same value -- see
    // getSequencerCellDeclaredLengthSteps() below.
    int getSequencerNaturalLengthSteps (int row) const;

    // This cell's own declared length in steps: its Step-extension
    // override if longer than natural, else natural -- the SAME value
    // SequencerGrid's piano-roll bar starts from before that bar gets cut
    // short for monophony (the next active cell elsewhere in the grid).
    // Tape Stop's decel duration in Sequenced mode is driven directly by
    // this (converted to host samples), deliberately NOT by how much
    // pattern happens to follow the step -- see its use in processBlock().
    int getSequencerCellDeclaredLengthSteps (int row, int column) const;

    // Currently selected drawing style (Step 41) -- persistent UI state
    // for the Style Palette: whichever swatch was last clicked is what
    // subsequent clicks/drags on the grid paint with. Defaults to Forward
    // (index 0), matching every other style-related default in this
    // codebase (Forward-only, byte-identical-until-touched).
    int getSelectedDrawingStyle() const { return selectedDrawingStyle.load(); }

    void setSelectedDrawingStyle (int style)
    {
        selectedDrawingStyle.store (juce::jlimit (0, numPlaybackStyleOptions - 1, style));
    }

    // Clear Sequence (Step 41): wipes the pattern back to all-empty, no
    // generation afterward -- the same wipe randomizeSequence() itself
    // starts with, just without anything following it.
    void clearSequence();

    // Randomize Sequence (Step 38/40/41): clears the pattern, then
    // randomly activates cells across all available rows/columns via fair
    // round-robin passes (Step 40 -- see randomizeSequence()'s own
    // implementation comment for why). Each placed hit treats its own
    // row's slice-length-in-steps as an exclusion zone -- no other hit may
    // land in the columns that slice would still be ringing out in --
    // and its PlaybackStyle is drawn from the same weighted
    // playbackStyleProbabilities table Slice Length/Clock modes already
    // use (Step 41), so turning a style's weight down elsewhere also
    // makes Randomize reach for it less often here. Simple constraint-
    // aware placement, not a "smart" generative algorithm -- it just
    // avoids obviously-wrong overlaps.
    void randomizeSequence();

    //=== Sequencer step parameter overrides (Step 45/46, Sequenced mode only) ===
    // Each sequencer cell can optionally carry parameter overrides -- a
    // sparse map from parameter name to value, populated only for cells
    // whose style actually uses that parameter. An empty/absent entry
    // means "use the global default," exactly like today. Built
    // generically (a list of parameter names, looked up by index) so
    // adding more later is just adding entries here, not rebuilding the
    // mechanism -- Step 45 proved this with Resonance alone (only Filter
    // Down/Up steps got a menu offering it); Step 46 adds Filter Type
    // (Filter Down/Up), Curve Shape (Ping-Pong/Tape Stop), and Grain
    // Size/Grain Speed (Stretch), each still only offered by the styles
    // that actually use them -- see getApplicableSequencerCellParameters()
    // below and SequencerGrid::showParameterMenuForCell().
    // Overrides for a given cell are cleared whenever that cell's own
    // style is set/changed (including cleared to empty) via
    // setSequencerCell(), and wiped entirely whenever the grid itself
    // resets (dimension change) or Clear/Randomize Sequence runs.
    // Step 47 adds Subdivide (index 5) -- see its own comment below;
    // unlike every other entry here it's GENERAL (offered on any active
    // step regardless of style), not gated by
    // getApplicableSequencerCellParameters()'s per-style table.
    // Step 49 adds Bitcrush's Sample Rate Reduction (index 6) and Bit
    // Depth (index 8), each paired with its own hidden Mode index (7 and
    // 9 respectively -- always index+1) storing Static/Sweep In/Sweep Out.
    // The Mode index is a normal discrete parameter in every other sense
    // (isSequencerCellParameterDiscrete(), NumOptions, OptionName all work
    // on it same as Filter Type/Curve Shape) but is never offered directly
    // by getApplicableSequencerCellParameters() -- see isSequencerCellParameterSwept()
    // and SequencerGrid::showParameterMenuForCell()'s swept branch for how
    // picking a mode there also opens the paired value index's slider.
    // Step Scratch-v1 adds Rate (index 10) -- Scratch's own bounce-cycle
    // note value, offered only for Scratch steps (see
    // getApplicableSequencerCellParameters()). Discrete like Filter Type/
    // Curve Shape (a plain right-click submenu listing the shared
    // note-value palette directly), deliberately NOT the stepped-slider
    // treatment Subdivide gets -- see isSequencerCellParameterDiscrete()/
    // isSequencerCellParameterSteppedSlider() below.
    // Scratch v2 adds Forward Curve (index 11) and Backward Curve (index
    // 12) -- which of the four shared EasingCurve shapes (see
    // EasingCurve.h) governs that stroke direction's own speed profile
    // within Scratch's bounce, independently per direction (see
    // processBlock()'s scratchActive branch and GranularStretcher::
    // foldPosition()'s forwardCurve/backwardCurve params). Discrete,
    // list-style submenus exactly like Rate -- their options are
    // EasingCurve's own four names, not the note-value palette.
    // Flanger adds Delay Time (index 13), Mix (index 15), and Feedback
    // (index 17), each paired with its own hidden Mode index (14, 16, and
    // 18 respectively -- always index+1), same "swept parameter with a
    // paired Mode index" shape as Bitcrush's Sample Rate Reduction/Bit
    // Depth above -- see isSequencerCellParameterSwept() and
    // SequencerGrid::showParameterMenuForCell()'s swept branch. Feedback
    // reuses the exact same Whole Window-aware sweep progress Delay
    // Time/Mix already compute (see processBlock()'s flangerProgress) --
    // no separate timing mechanism needed, it's just a third value fed
    // through the same sweptFlangerValue lambda.
    // Volume (index 19) adds a style-independent ramp, paired with its own
    // hidden Mode index (20) -- same "swept value + paired Mode" shape as
    // Bitcrush/Flanger above, but GENERAL rather than gated by a specific
    // style, same as Subdivide (index 5): appended unconditionally by
    // getApplicableSequencerCellParameters() for every active step
    // regardless of PlaybackStyle. Its Mode uses its own directional
    // option names ("Static"/"Ramp Up"/"Ramp Down", see
    // getSequencerCellParameterOptionName()) rather than the shared
    // Sweep In/Out sweepModeNames every other swept parameter's Mode uses,
    // since volume has an intuitive up/down sense the others don't --
    // Ramp Up/Down always sweep toward/away from silence (0.0), a fixed
    // "extreme" like every other swept parameter has, not a second
    // user-adjustable value. The slider sets the reference level (Ramp
    // Down's start / Ramp Up's target), same interaction as Static. It's
    // an ADDITIONAL multiplier layered onto the existing base fade-in/out
    // gain (see processBlock()'s volumeGain), not a replacement for it,
    // and reuses Flanger's exact Whole Window progress mechanism
    // (samplesSinceWindowStart/currentWindowLengthHostSamples) so a
    // Subdivide-d step's ramp glides smoothly across the whole step
    // rather than resetting on every retrigger -- see
    // processBlock()'s volumeUseWholeWindow. No global dial (same as
    // Subdivide) -- see getSequencerCellParameterGlobalValue()'s own doc
    // comment -- so it's Sequenced-mode-only, not offered in Slice
    // Length/Clock mode.
    // Send-to-bus (Pass 2) adds Delay Send Amount (21), Delay Bus Time (22),
    // Delay Bus Feedback (23), Reverb Send Amount (24), Reverb Bus Size
    // (25), and Reverb Bus Decay (26) -- deliberately distinctly NAMED from
    // Flanger's own "Delay Time"/"Feedback" (indices 13/17), since the
    // per-cell override map is keyed by NAME, shared across every parameter
    // a cell might carry -- a step using Flanger AND sending to the Delay
    // bus must be able to set both independently without collision. All
    // six are general (style-independent, like Subdivide/Volume) but,
    // unlike those two, are deliberately NOT returned by
    // getApplicableSequencerCellParameters() -- SequencerGrid's right-click
    // menu offers them itself, grouped into "Send to Delay"/"Send to
    // Reverb" submenus (see its own showParameterMenuForCell()), rather
    // than as flat top-level entries. Send Amount (21/24) has no global
    // dial (0.0f fallback, same as Subdivide/Volume -- see
    // getSequencerCellParameterGlobalValue()); Delay Bus Time/Feedback and
    // Reverb Bus Size/Decay (22/23/25/26) DO have one -- the shared bus's
    // own CURRENT (possibly still-ramping) value, read live via
    // getDelayBusTimeMs() etc., since this is genuinely one shared,
    // continuously-reconfigured bus, not a per-step instance (see this
    // feature's own top-level doc comment and processSendBuses()).
    static constexpr int numSequencerCellParameters = 27;
    static juce::String getSequencerCellParameterName (int index); // "Resonance" / "Filter Type" / "Curve Shape" / "Grain Size" / "Grain Speed" / "Subdivide" / "Sample Rate Reduction" / "Sample Rate Reduction Mode" / "Bit Depth" / "Bit Depth Mode" / "Rate" / "Forward Curve" / "Backward Curve" / "Delay Time" / "Delay Time Mode" / "Mix" / "Mix Mode" / "Feedback" / "Feedback Mode" / "Volume" / "Volume Mode" / "Delay Send Amount" / "Delay Bus Time" / "Delay Bus Feedback" / "Reverb Send Amount" / "Reverb Bus Size" / "Reverb Bus Decay"

    // Swept parameters (Step 49; Volume): true for indices 6 and 8 (Sample
    // Rate Reduction, Bit Depth), 13, 15, 17 (Flanger's Delay Time, Mix,
    // Feedback), and 19 (Volume) -- these open a mode-choice submenu FIRST
    // (Static/Sweep In/Sweep Out, or for Volume, Static/Ramp Up/Ramp
    // Down), rather than going straight to a plain discrete-options
    // submenu or straight to the slider overlay the way every other
    // parameter here does. See SequencerGrid::showParameterMenuForCell().
    static bool isSequencerCellParameterSwept (int index);

    // Step 46: Resonance/Grain Size/Grain Speed are continuous (drive the
    // existing slider overlay); Filter Type/Curve Shape instead present
    // as a small selectable list directly from the right-click menu (see
    // SequencerGrid::showParameterMenuForCell) -- no slider makes sense
    // for a handful of named choices. These three describe which is
    // which and, for the list-style ones, what their options are.
    // Subdivide (Step 47) is also discrete (its options are the shared
    // note-value palette, plus "Off") but is NOT list-style -- see
    // isSequencerCellParameterSteppedSlider() just below.
    static bool isSequencerCellParameterDiscrete (int index);
    static int getSequencerCellParameterNumOptions (int index); // only meaningful when isSequencerCellParameterDiscrete() is true
    static juce::String getSequencerCellParameterOptionName (int index, int optionIndex);

    // Subdivide (Step 47) alone: discrete like Filter Type/Curve Shape
    // (a fixed list of named options, not an arbitrary range), but
    // presented via the SAME drag-slider overlay Resonance/Grain Size/
    // Grain Speed use rather than a plain list submenu -- a note-value
    // rate is naturally ordered (Off, then fastest to slowest, or
    // vice versa) in a way Filter Type/Curve Shape's options aren't, so
    // dragging across stops reads naturally. The slider just SNAPS to
    // one of getSequencerCellParameterNumOptions()'s stops instead of an
    // arbitrary value -- see SequencerGrid::updateEditingValueFromMouseX().
    static bool isSequencerCellParameterSteppedSlider (int index);

    // Continuous parameters' slider range (Step 46) -- generalizes the
    // slider overlay's value mapping, which used to hardcode Resonance's
    // own range as the only option. Meaningless (returns a harmless 0/1
    // placeholder) for discrete parameters, which never reach the slider
    // (Subdivide is the one exception -- see isSequencerCellParameterSteppedSlider()
    // above -- but it's stepped by option INDEX, not this min/max range).
    static float getSequencerCellParameterMin (int index);
    static float getSequencerCellParameterMax (int index);

    // Which of the parameters above are relevant to a given cell style
    // (PlaybackStyle ordinal, matching indexToPlaybackStyle()'s own
    // numbering, and the same ordinal sequencerGrid itself stores) --
    // e.g. a Forward step offers none, Filter Down/Up offers Resonance +
    // Filter Type. An out-of-range/empty-cell style (-1) offers none.
    // Subdivide (Step 47) and Volume (index 19) are deliberately NOT
    // included here -- both are appended unconditionally by this function
    // itself for every valid (non-empty-cell) style, since neither is
    // tied to any one effect the way everything else here is (Subdivide
    // is a general retrigger mechanism; Volume is a pure gain stage that
    // applies identically regardless of which style's DSP is running).
    static std::vector<int> getApplicableSequencerCellParameters (int style);

    // This parameter's current GLOBAL value (i.e. what applies when no
    // per-step override exists) -- used as the slider overlay's fallback/
    // starting value, generalizing the single getFilterSweepResonance()
    // call Step 45 used directly. Not static (unlike the helpers above)
    // since it reads live atomic state.
    float getSequencerCellParameterGlobalValue (int index) const;

    // Writes this parameter's GLOBAL value (the mirror-image dispatcher of
    // getSequencerCellParameterGlobalValue() above) -- used by the Slice
    // Length/Clock mode parameter panel, which edits global defaults
    // directly rather than a per-step override. Discrete/Mode parameters
    // are written as their option index cast to float, same convention
    // setSequencerCellParameterOverride() already uses. A no-op for
    // Subdivide (index 5) and Volume/Volume Mode (indices 19/20), none of
    // which have a global dial (see getSequencerCellParameterGlobalValue()'s
    // own doc comment).
    void setSequencerCellParameterGlobalValue (int index, float value);

    bool getSequencerCellHasParameterOverride (int row, int column, const juce::String& parameterName) const;
    float getSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float fallbackValue) const;
    void setSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float value);

    // True if the cell has ANY parameter override at all -- drives the
    // small corner marker SequencerGrid draws on customized steps.
    bool getSequencerCellHasAnyParameterOverride (int row, int column) const;

    // Lock-free copy of the currently active step column, for the UI's
    // playhead indicator on the sequencer grid -- same pattern as
    // currentlyPlayingSliceIndexForUI/the Audition playhead. -1 when
    // Sequenced mode isn't active (or transport stopped).
    int getCurrentlyPlayingStepIndex() const { return currentlyPlayingStepIndexForUI.load(); }

    //=== MIDI input / Sequencer pattern bank (Pass 1: immediate recall; Pass 2: Set Interval/End of Pattern timing) ===
    // A small, general dispatch layer reads every incoming MidiBuffer in
    // processBlock() and routes note-on events by current TriggerMode (see
    // dispatchNoteOn() in the private section below) -- Sequenced mode's
    // pattern bank is the only handler today, but adding a future Perform
    // or MIDI Control mode is just another case in that switch, not a
    // rewrite of the read/dispatch plumbing itself.
    //
    // The bank has 128 slots, indexed 1:1 by MIDI note number. Each slot
    // either holds a complete snapshot of the Sequencer grid (every cell's
    // style, every parameter override, and the step-resolution/pattern-
    // length that define the grid's own dimensions) or is empty. Slots are
    // populated lazily, only via MIDI Learn: armMidiLearnForPatternSave()
    // captures the CURRENT grid immediately (not whenever the note
    // eventually arrives), then the next note-on received while armed
    // claims that slot. For an empty slot, a recall note-on is always a
    // silent no-op -- whatever's currently playing is left completely
    // undisturbed. None of this is persisted across DAW sessions yet (see
    // getStateInformation()'s own doc comment).
    void armMidiLearnForPatternSave(); // captures the current grid; takes sampleLock itself -- UI-thread entry point
    void cancelMidiLearn();
    bool isMidiLearnArmed() const { return midiLearnArmed.load(); }

    // One locked snapshot copy per call, cheap enough for a UI timer to
    // poll at a modest rate without hammering sampleLock 128 times a tick.
    std::array<bool, 128> getPopulatedPatternBankSlots() const;

    // -1 if no slot has been recalled this session (still whatever the user
    // last drew/edited by hand).
    int getActivePatternBankSlot() const { return activePatternBankSlot.load(); }

    // Pattern Switch Timing (Pass 2) -- governs WHEN a recall note-on for a
    // populated slot actually takes effect. Purely a timing layer on top of
    // the recall mechanism above; doesn't touch Trigger Mode, Pitch Mode, or
    // any playback style logic.
    //   immediate    -- unchanged from Pass 1: switches the instant the
    //     note-on arrives, mid-block if need be.
    //   setInterval  -- defers the switch to the next occurrence of a
    //     chosen musical grid point (patternSwitchIntervalIndex, same
    //     note-value palette as Clock reference/Step resolution), checked
    //     every sample against host ppq (see processBlock()'s sequencedMode
    //     branch) -- same per-sample boundary discipline Clock mode and the
    //     mandatory Reset feature already use.
    //   endOfPattern -- defers the switch to the moment the CURRENTLY
    //     PLAYING pattern wraps at its own Pattern Length -- reuses that
    //     pattern's existing step-wrap detection as the trigger, no
    //     separate ppq math.
    // In either deferred mode, a new note-on before the boundary replaces
    // the pending target outright (last note before the boundary wins) --
    // never more than one switch pending at a time.
    enum class PatternSwitchTiming { immediate, setInterval, endOfPattern };

    void setPatternSwitchTiming (PatternSwitchTiming timing)
    {
        patternSwitchTiming.store (timing);
        pendingPatternSwitchNote.store (-1); // changing the timing mode abandons any switch already pending under the old one
    }

    PatternSwitchTiming getPatternSwitchTiming() const { return patternSwitchTiming.load(); }

    // Set Interval's grid point -- same numNoteValueOptions palette as
    // Clock reference/Step resolution (see getNoteValueName()/
    // getNoteValueBeats() above). Meaningless while Immediate or End of
    // Pattern is selected, same "stored but inert unless its mode is
    // active" convention clockReferenceIndex etc. already follow.
    void setPatternSwitchIntervalIndex (int index)
    {
        patternSwitchIntervalIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getPatternSwitchIntervalIndex() const { return patternSwitchIntervalIndex.load(); }

    // -1 if no switch is currently pending; otherwise the MIDI note number
    // (== pattern bank slot) a deferred switch is headed toward, for the UI
    // to show as "pending," distinct from "active." Only ever non-(-1)
    // while patternSwitchTiming is setInterval or endOfPattern.
    int getPendingPatternSwitchSlot() const { return pendingPatternSwitchNote.load(); }

    //=== Performance mode state bank (click-to-focus + auto-save) ===
    // A 128-note-indexed, lazily-populated bank, same shape as the
    // Sequencer pattern bank above, reused via dispatchNoteOn()'s own
    // TriggerMode::performance case -- but each slot is a self-contained
    // PERFORMANCE STATE rather than a grid: one hand-trimmed segment, one
    // PlaybackStyle, that style's own independent 21-parameter values (NOT
    // the global defaults Slice Length/Clock use, NOT Sequenced mode's
    // per-cell overrides -- genuinely independent storage, since Performance
    // mode must stay fully independent of the other three), a Loop toggle,
    // and a Sync toggle (on: follows whichever global Pitch Mode is active;
    // off: native/unsynced playback -- see processBlock()'s
    // currentPickNativeRateActive).
    //
    // Slots are no longer assigned via MIDI Learn -- editing focus (which
    // slot the parameter panel/trim handles are currently shaping) is set
    // exclusively by clicking a key on Performance mode's on-screen keyboard
    // (setFocusedPerformanceStateSlot(), below), and physical MIDI is
    // playback-only (see handlePerformanceStateNoteOn() in the private
    // section). Trim is deliberately NOT duplicated as a separate "working"
    // value while a slot has focus -- editing happens on the same shared
    // trimStartSample/trimEndSample atomics + waveform trim handles every
    // other mode already uses; only each SAVED slot gets its own
    // independent copy, captured automatically the instant focus moves to a
    // different slot (setFocusedPerformanceStateSlot()'s auto-save) and
    // restored (or defaulted, for a never-before-focused slot) the instant
    // focus moves back in -- exactly the same "one shared live surface,
    // many independent saved snapshots of it" shape the Sequencer pattern
    // bank already uses for sequencerGrid.
    struct PerformanceStateSnapshot
    {
        bool populated = false;
        int trimStartSample = 0, trimEndSample = 0;
        int style = 0; // PlaybackStyle index
        std::array<float, numSequencerCellParameters> parameterValues {}; // indexed exactly as getSequencerCellParameterName()
        bool loop = false;
        bool sync = true;
    };

    // Click-to-focus (replaces the old MIDI-Learn "Save to..." button +
    // arm-and-assign flow entirely): moves editing focus to noteNumber,
    // auto-saving whatever was being edited in the previously-focused slot
    // first, then loading noteNumber's own saved state (or a fresh default,
    // if it has none yet) into performanceWorkingState + the shared trim
    // atomics. UI-thread entry point (the on-screen keyboard's click
    // handler); takes sampleLock itself.
    void setFocusedPerformanceStateSlot (int noteNumber);
    int getFocusedPerformanceStateSlot() const { return focusedPerformanceStateSlot.load(); } // -1 = nothing focused yet

    std::array<bool, 128> getPopulatedPerformanceStateBankSlots() const;

    // Quantize Recall -- same mechanism as Pattern Switch Timing's Set
    // Interval mode above (see that enum's own doc comment), applied to
    // Performance mode's MIDI state recall instead of the Sequencer pattern
    // bank. Off (immediate, the original behaviour) by default. When on, a
    // physical MIDI key press -- recalling any saved state, or
    // live-auditioning the currently-focused one -- doesn't switch right
    // away; it arms pendingPerformanceRecallNote and waits for the next
    // occurrence of performanceQuantizeRecallIntervalIndex's grid point,
    // checked every SAMPLE against host ppq in processBlock()'s
    // performanceMode branch -- the same per-sample boundary discipline
    // Set Interval, Clock mode, and the mandatory Reset feature all already
    // use, avoiding the Step 6 bug (a boundary computed once per block
    // silently missing one that lands mid-block). A newer note-on before
    // that point just overwrites pendingPerformanceRecallNote in
    // handlePerformanceStateNoteOn(), so the newest press always wins --
    // never more than one recall pending at a time, same rule Set Interval
    // itself follows.
    //
    // Transport-independence: falls back to immediate if the host transport
    // isn't playing when the key is pressed (there's no meaningful beat
    // position to quantize against without it), and also if the transport
    // stops while a switch is already pending (see processBlock()'s own
    // handling) -- both preserve the "auditionable without pressing play"
    // behaviour Performance mode already has.
    void setPerformanceQuantizeRecallEnabled (bool enabled)
    {
        performanceQuantizeRecallEnabled.store (enabled);
        pendingPerformanceRecallNote.store (-1); // changing the setting abandons any switch already pending under the old one, same as setPatternSwitchTiming()
    }

    bool getPerformanceQuantizeRecallEnabled() const { return performanceQuantizeRecallEnabled.load(); }

    // Quantize Recall's grid point -- same numNoteValueOptions palette as
    // Clock reference/Set Interval (see getNoteValueName()/
    // getNoteValueBeats() above). Meaningless while Quantize Recall is off,
    // same "stored but inert unless its mode is active" convention
    // patternSwitchIntervalIndex etc. already follow.
    void setPerformanceQuantizeRecallIntervalIndex (int index)
    {
        performanceQuantizeRecallIntervalIndex.store (juce::jlimit (0, numNoteValueOptions - 1, index));
    }

    int getPerformanceQuantizeRecallIntervalIndex() const { return performanceQuantizeRecallIntervalIndex.load(); }

    // -1 if no recall is currently pending; otherwise the MIDI note number
    // (== performance state bank slot) a deferred recall is headed toward.
    // Only ever non-(-1) while performanceQuantizeRecallEnabled is true.
    int getPendingPerformanceRecallSlot() const { return pendingPerformanceRecallNote.load(); }

    // The "working state" -- style/params/loop/sync currently being edited
    // via performanceStyleParameterPanel/the Loop+Sync toggles, ahead of
    // whatever the next Save captures. Parameter values are seeded once, in
    // the constructor, from getSequencerCellParameterGlobalValue() (sane
    // starting values, not zeros -- zero is a broken default for parameters
    // like Bit Depth); independent storage from that point on.
    int getPerformanceWorkingStyle() const;
    void setPerformanceWorkingStyle (int style);
    float getPerformanceWorkingParameterValue (int index) const;
    void setPerformanceWorkingParameterValue (int index, float value);
    bool getPerformanceWorkingLoop() const;
    void setPerformanceWorkingLoop (bool loop);
    bool getPerformanceWorkingSync() const;
    void setPerformanceWorkingSync (bool sync);

    //=== Control mode (piano-roll slice triggering with keyswitch style selection) ===
    // Reuses the shared MIDI dispatch layer (dispatchNoteOn()'s own switch,
    // above) directly -- see handleControlNoteOn()/handleControlNoteOff() in
    // the private section for the actual dispatch.
    //
    // Base note (default MIDI 36 == C1, Ableton/Simpler's own octave
    // numbering, where middle C is C3) marks where the ascending-chromatic
    // slice range starts; slice range is capped at numSequencerRows (32),
    // same cap as the Sequencer's own rows, for consistency across the
    // plugin. Keyswitch notes are a fixed, hardcoded block immediately below
    // the base note -- see getControlKeyswitchNote() below -- no assignable
    // storage at all, so moving the base note shifts the whole keyswitch
    // block with it automatically.
    void setControlBaseNote (int noteNumber) { controlBaseNote.store (juce::jlimit (0, 127, noteNumber)); }
    int getControlBaseNote() const { return controlBaseNote.load(); }

    // Trigger (default, matching every other mode's own trigger behaviour
    // and Simpler's own default) ignores note-off entirely, playing each
    // pick to its natural/effect-driven completion. Gate stops playback the
    // instant the triggering note is released, via the same fadeOutMs
    // click-avoidance ramp used everywhere else in the engine (see
    // controlGateReleaseActive/controlGateReleaseElapsedSamples below) --
    // a genuinely new kind of interruption, since nothing else here stops
    // playback from an external signal mid-pick.
    void setControlGateMode (bool gateEnabled) { controlGateModeActive.store (gateEnabled); }
    bool getControlGateMode() const { return controlGateModeActive.load(); }

    // Fixed keyswitch mapping: playbackStyleIndex's dedicated note is always
    // baseNote - 1 - playbackStyleIndex, i.e. the numPlaybackStyleOptions
    // notes immediately below the base note, in the same order as the
    // playback-style palette/probability table (Forward closest to the base
    // note, Flanger furthest below it). Pure arithmetic -- no per-session
    // state, nothing to assign, nothing that can drift out of sync with the
    // base note. May fall outside [0, 127] for a low enough base note (a
    // genuinely unreachable keyswitch, same as any MIDI mapping that runs
    // off the end of the note range) -- callers displaying this should treat
    // an out-of-range result as "unreachable," not clamp it into range.
    int getControlKeyswitchNote (int playbackStyleIndex) const
    {
        return controlBaseNote.load() - 1 - playbackStyleIndex;
    }

    // Which PlaybackStyle index the last keyswitch note-on selected --
    // Forward (0) until any keyswitch has ever fired this session.
    int getControlActiveStyle() const { return controlActiveStyle.load(); }

#if JUCE_DEBUG
    // TEMPORARY DEBUG -- remove once step-extension Tape Stop testing is
    // done. Call from a UI-thread timer (SequencerGrid's own 30fps poll)
    // to drain and print whatever Tape Stop diagnostic events the audio
    // thread queued up since the last call. Does the actual DBG()/console
    // I/O itself -- entirely off the audio thread, never touches
    // sampleLock -- see the mailbox members' own doc comment for why this
    // exists instead of calling DBG() directly from processBlock().
    void drainDebugTapeStopEvents();

    // TEMPORARY DEBUG (Stretch step-extension verification) -- same
    // pattern/lifecycle as drainDebugTapeStopEvents() above, just for the
    // Stretch pick-start mailbox instead.
    void drainDebugStretchEvents();
#endif

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
        if (index == 4) return PlaybackStyle::filterSweepDown;
        if (index == 5) return PlaybackStyle::filterSweepUp;
        if (index == 6) return PlaybackStyle::bitcrush;
        if (index == 7) return PlaybackStyle::scratch;
        if (index == 8) return PlaybackStyle::flanger;
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

    // Scratch (v1): this pick's bounce-cycle length (one full forward-
    // backward cycle at its own Rate note value), in HOST samples --
    // shared by every trigger mode's own pick-start duration/currentEndSample
    // calculation and, converted back to source-domain samples via
    // playbackRate, the fold length actually passed to
    // GranularStretcher::foldPosition() (see processBlock()'s shared
    // bounceFoldLengthSamples). Clamped so one LEG of the cycle (half of
    // it) never exceeds the slice's own content length -- Rate is
    // tempo-synced and completely independent of slice length, so an
    // unclamped cycle could otherwise ask foldPosition to bounce past the
    // slice's actual audio into whatever follows it in the buffer.
    // Degenerate (<= 0) sliceLength/hostBpm/hostSampleRate/playbackRate
    // returns 0.0 rather than dividing by zero -- callers already treat a
    // zero cycle length as a harmless no-op (foldPosition's own
    // sliceLength <= 0.0 guard falls back to plain Forward).
    static double computeScratchCycleLengthHostSamples (int rateIndex, int sliceLength,
                                                          double hostBpm, double hostSampleRate, double playbackRate);

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
    // bring it back. Must be called with sampleLock already held. Also
    // where Quantize Transients (Step 35) plugs in -- see
    // quantizeOnsetToGrid() below, called on each surviving auto-detected
    // onset before it's merged with manual points.
    std::vector<Slice> mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices, int trimStart, int trimEnd) const;

    // Quantize detected transients to grid (Step 35) -- snaps a single
    // auto-detected onset's sample position to the nearest Grid step.
    // trimStart is passed in (rather than re-read from trimStartSample)
    // since the caller (mergeOnsetsIntoSlices) already has it and both
    // must agree on the same value within one merge pass. Returns
    // onsetSample UNCHANGED if the source tempo is degenerate (<= 0 BPM)
    // -- same defensive fallback style already used by
    // computeBeatQuantizeTarget() for the analogous per-pick feature.
    // Clamped into [trimStart, trimEnd) afterward so an onset near the
    // very edge of the trim can never quantize to a position outside it.
    int quantizeOnsetToGrid (int onsetSample, int trimStart, int trimEnd) const;

    // Trim Snap mode (Grid) -- true only while Performance mode is the
    // active trigger mode AND performanceTrimSnapMode is Grid. Checked by
    // setTrimStartSample()/setTrimEndSample() to pick findNearestGridSample()
    // over the usual transient search; kept as its own tiny helper since
    // both call sites need the exact same condition.
    bool shouldGridSnapTrim() const
    {
        return triggerMode.load() == TriggerMode::performance
            && performanceTrimSnapMode.load() == TrimSnapMode::grid;
    }

    // Trim Snap mode (Grid) -- finds the nearest FIXED musical grid position
    // to rawSample, spaced by getNoteValueBeats(performanceTrimGridIndex) at
    // the sample's established tempo (getCalculatedOriginalBpm()), anchored
    // at tempoTrimStartSample -- the one stable "beat 0" reference that
    // stays put regardless of which Performance state slot currently has
    // focus (see tempoTrimStartSample's own comment). Same beats<->samples
    // round-trip quantizeOnsetToGrid() above already uses for the analogous
    // auto-detected-transient-quantize feature, just anchored differently:
    // that one anchors to the trim it's searching WITHIN, which isn't
    // available here since the trim handle itself is what's moving --
    // tempoTrimStartSample is the nearest equivalent stable reference.
    // Returns rawSample unchanged if the source tempo or grid resolution is
    // degenerate (<= 0), same defensive fallback quantizeOnsetToGrid() uses.
    // NOT clamped to any range itself -- both callers already clamp the
    // result into the allowed handle range, same as they already do for the
    // transient-snap search.
    int findNearestGridSample (int rawSample) const;

    // Sequenced Trigger Mode (Step 37) -- clears the pattern to all-off,
    // sized to the CURRENT grid dimensions (getSequencerNumRows()/
    // getSequencerNumSteps()). Called whenever either dimension changes:
    // slices rebuild (rows), or loop length/step resolution change
    // (columns). Must be called with sampleLock already held.
    void resetSequencerGrid();

    // One pattern bank slot's full contents (Step: MIDI pattern bank) --
    // see captureCurrentSequencerPatternSnapshot()/patternBank below for how
    // it's populated and read.
    struct SequencerPatternSnapshot
    {
        bool populated = false;
        int rows = 0, columns = 0;
        int stepResolutionIndex = 0, patternLengthBarsIndex = 0;
        std::vector<int> grid;
        std::map<int, std::map<juce::String, float>> parameterOverrides;
        std::map<int, int> extendedLengthSteps;
    };

    // MIDI input dispatch layer -- see the public "MIDI input / Sequencer
    // pattern bank" section above for the overall design. All of these,
    // plus completeMidiLearn()/handleSequencerPatternRecallNoteOn() below,
    // are only ever called from within processBlock() and assume sampleLock
    // is already held by the caller (same convention resetSequencerGrid()
    // itself follows) -- they must never take the lock themselves.
    // hostTransportPlaying is threaded through from processBlock() (computed
    // there ahead of this call, see its own call site) purely so Quantize
    // Recall's note-on handler can decide immediate-vs-deferred on the spot
    // -- every other handler ignores it.
    void handleIncomingMidi (const juce::MidiBuffer& midiMessages, bool hostTransportPlaying);
    void dispatchNoteOn (int noteNumber, float velocity01, bool hostTransportPlaying);

    // Note-off dispatch -- only ever meaningful to Control mode's Gate
    // setting (see handleControlNoteOff() below); every other mode still
    // ignores note-off entirely, unchanged from before this existed.
    void dispatchNoteOff (int noteNumber);

    // Sequenced mode's recall entry point (Pass 2) -- routes by
    // patternSwitchTiming: immediate applies handleSequencerPatternRecallNoteOn()
    // below right away (unchanged Pass 1 behaviour); setInterval/endOfPattern
    // instead arm pendingPatternSwitchNote and let the per-sample boundary
    // check in processBlock()'s sequencedMode branch apply the switch once
    // the chosen boundary is crossed. Empty slot -> no-op either way, same
    // as Pass 1.
    void handlePatternSwitchNoteOn (int noteNumber);

    // The actual grid swap, called either directly (Immediate) or from the
    // deferred per-sample boundary check (Set Interval/End of Pattern). A
    // populated slot overwrites the live grid AND its defining dimensions
    // (stepResolutionIndex/patternLengthBarsIndex) wholesale, then forces
    // sequencedModeInitialized false so the step-boundary tracker re-syncs
    // against the new grid on the very next check -- the same "just
    // switched, trigger immediately" mechanic setTriggerMode() already
    // relies on for a mode change. Callers invoking this MID-BLOCK (the
    // deferred timing modes) must also set sequencedModeInitialized back to
    // true afterward once they've re-derived this sample's step alignment
    // themselves -- see the sequencedMode branch in processBlock() -- since
    // otherwise the NEXT block would re-trigger the same step a second time
    // (sequencedModeInitialized only gets consulted once per block, and
    // this call happens after that block's own check already ran).
    void handleSequencerPatternRecallNoteOn (int noteNumber);

    // Writes pendingSaveSnapshot (captured back when armMidiLearnForPatternSave()
    // was clicked) into patternBank[noteNumber] and clears midiLearnArmed.
    void completeMidiLearn (int noteNumber);

    // Captures rows/columns/stepResolutionIndex/patternLengthBarsIndex
    // alongside the grid+override maps themselves -- those two indices
    // define the grid's column stride, so a recall that restored the cell
    // data without them would reinterpret it against the wrong stride and
    // scramble every cell. Assumes sampleLock already held.
    SequencerPatternSnapshot captureCurrentSequencerPatternSnapshot() const;

    // Performance mode's own note-on entry point -- dispatched from
    // dispatchNoteOn()'s TriggerMode::performance case. An empty, unfocused
    // slot is a no-op regardless of Quantize Recall, same as Pass 1 (checked
    // up front here before either path below runs). Otherwise routes by
    // performanceQuantizeRecallEnabled, exactly the same "immediate vs.
    // arm-and-defer" split handlePatternSwitchNoteOn() uses for
    // patternSwitchTiming:
    //   off, OR on but the host transport isn't playing right now (no
    //     meaningful beat position to quantize against without it) --
    //     applies immediately via applyPerformanceStateRecall(), unchanged
    //     Pass 1 behaviour.
    //   on AND transport playing -- arms pendingPerformanceRecallNote and
    //     lets the per-sample boundary check in processBlock()'s
    //     performanceMode branch apply it once the chosen grid point is
    //     reached. A newer note-on before that point just overwrites the
    //     same atomic, so the newest press always wins -- never more than
    //     one recall pending at a time.
    void handlePerformanceStateNoteOn (int noteNumber, bool hostTransportPlaying);

    // The actual focus/playback-source swap, called either directly
    // (Quantize Recall off, or transport stopped) or from the deferred
    // per-sample boundary check (Quantize Recall on). Re-checks the target
    // slot's populated flag for the non-focused case (mirrors
    // handleSequencerPatternRecallNoteOn()'s own re-check) since a deferred
    // call runs an arbitrary amount of time after the note-on that armed
    // it -- moved out of handlePerformanceStateNoteOn() itself so both call
    // sites (immediate and deferred) share exactly one implementation.
    void applyPerformanceStateRecall (int noteNumber);

    // Control mode's own note-on entry point -- dispatched from
    // dispatchNoteOn()'s TriggerMode::control case. Keyswitch notes (the
    // fixed getControlKeyswitchNote() block, checked via the inverse
    // arithmetic below) are checked first -- a match just updates
    // controlActiveStyle and produces no sound. Otherwise noteNumber is
    // resolved against the ascending-chromatic slice range starting at
    // controlBaseNote (capped at numSequencerRows, same as the Sequencer);
    // outside both ranges is a silent no-op. The two ranges are structurally
    // disjoint by construction (keyswitches always sit below controlBaseNote,
    // slices always at or above it), so there's no overlap to resolve --
    // checking keyswitches first is purely a cheap-arithmetic-first
    // ordering, not overlap resolution. A resolved slice note arms
    // controlNoteOnPending, consumed on the very next per-sample check in
    // processBlock()'s controlMode branch -- same same-call, same-lock
    // handoff shape performanceRecallPending already uses.
    void handleControlNoteOn (int noteNumber, float velocity01);

    // Control mode's Gate release -- a no-op unless Gate mode is on and
    // noteNumber matches whichever note is actually sounding right now
    // (controlCurrentlySoundingNote); arms controlGateReleaseActive, which
    // the shared per-sample gain stage in processBlock() fades to silence
    // over fadeOutMs (the same click-avoidance envelope used everywhere
    // else) before forcing hasCurrentPick false.
    void handleControlNoteOff (int noteNumber);

    // Unifies the tempo math (Step 23) that both Trim markers and Manual
    // BPM override feed into:
    //   sourceSpanSeconds = manualBpmOverrideEnabled
    //       ? (loopLengthBars * 4 * 60) / manualBpmOverrideValue
    //       : (tempoTrimEndSample - tempoTrimStartSample) / sampleSampleRate
    // Used by both getCalculatedOriginalBpm() (the UI's "~X BPM" label) and
    // processBlock()'s repitchRatio — replaces the old calculation, which
    // used the whole buffer's length regardless of trim (the bug this
    // fixes). The existing repitchRatio formula itself (sourceSpanSeconds /
    // hostLoopLengthSeconds) is otherwise unchanged, and GranularStretcher
    // never computes tempo itself — it only ever receives the ratios
    // (repitchRatio, srConversionRatio) this feeds into, so it stays
    // consistent with the direct-read path "for free."
    //
    // Deliberately reads tempoTrimStartSample/tempoTrimEndSample, NOT the
    // plain trimStartSample/trimEndSample -- see tempoTrimStartSample's own
    // comment (Performance mode's per-state trim fix). The two pairs are
    // identical outside Performance mode; they only diverge once Performance
    // mode starts repointing the shared trim atomics at whichever state slot
    // has editing focus, which must NOT feed back into "the sample's
    // original tempo."
    double computeSourceSpanSeconds() const;

    // Tempo-relative minimum holdoff between consecutive detected
    // transients, replacing the old fixed defaultHoldoffMs floor everywhere
    // detection actually runs (see the call sites below). At max
    // sensitivity, a fixed ms floor lets detection density run away on fast
    // material and stay needlessly sparse on slow material — neither
    // bounded by anything musical. This instead never allows two onsets
    // closer than roughly a 32nd note apart AT THE LOOP'S OWN CALCULATED
    // TEMPO (getCalculatedOriginalBpm(), the same trim/bars/manual-override
    // -aware derivation used everywhere else tempo matters in this class),
    // so density scales with how fast the material actually is instead of
    // an arbitrary constant. A 32nd note is 1/8 of a quarter-note beat
    // (assumes 4/4, same assumption used throughout); 60000/bpm is one
    // beat in ms.
    //
    // Falls back to the old fixed defaultHoldoffMs when there's no usable
    // tempo yet (bpm <= 0 -- no sample loaded, or a degenerate span), so
    // behaviour before a sample loads is unchanged. Also floors at 1ms as a
    // numerical safety net (not a musical one) against an absurd manual BPM
    // override collapsing the holdoff to ~0 and effectively disabling it.
    float computeMinimumHoldoffMs() const
    {
        const double bpm = getCalculatedOriginalBpm();

        if (bpm <= 0.0)
            return defaultHoldoffMs;

        constexpr double thirtySecondNoteFractionOfBeat = 1.0 / 8.0;
        const double beatMs = 60000.0 / bpm;
        return (float) juce::jmax (1.0, beatMs * thirtySecondNoteFractionOfBeat);
    }

    // Audition (Step 25) — the raw, generative-engine-bypassing loop
    // render. Called from renderPickBlock() (sampleLock already held) in
    // place of everything below it whenever auditionActive is set. Reads/
    // writes auditionPosition; safe from the UI thread too only because
    // setAuditionActive() takes the same lock.
    void renderAudition (juce::AudioBuffer<float>& buffer, double hostSampleRate);

    // The entire former processBlock() body, renamed and otherwise
    // unchanged -- every early-return gate (no sample loaded, Audition
    // mode, no slices, transport-stopped resets, invalid BPM/sample rate)
    // and the whole single-voice per-sample render loop still live here
    // exactly as before, filling `buffer` with this block's dry output (or
    // silence, via those same early returns). processBlock() itself is now
    // just this call followed by processSendBuses() below, so the Delay/
    // Reverb buses keep running -- and any ringing tail keeps decaying --
    // even on a block where this renders nothing at all.
    void renderPickBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages);

    // Feeds a scaled copy of `buffer` (this block's just-rendered dry
    // output) into the Delay and Reverb buses' own persistent DSP state,
    // then mixes each bus's wet output back into `buffer`, scaled by that
    // bus's own Return Level. Called unconditionally at the end of every
    // processBlock() call, audio-thread-only (no lock needed -- nothing
    // else touches this state).
    void processSendBuses (juce::AudioBuffer<float>& buffer);

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

    // Tempo trim (Performance mode Pass 1 fix) — a second copy of the trim
    // markers, updated in lockstep with trimStartSample/trimEndSample by
    // every REAL trim edit (setTrimStartSample()/setTrimEndSample() while
    // outside Performance mode) and by loadSample()'s reset, but otherwise
    // left alone. This is what computeSourceSpanSeconds() actually reads
    // (see its own comment) — the sample's one true, already-established
    // tempo basis, kept stable even while Performance mode repoints the
    // shared trimStartSample/trimEndSample atomics at whichever state slot
    // currently has editing focus (see setFocusedPerformanceStateSlot() and
    // the "focused slot's segment IS the shared trim atomics" comment in
    // processBlock()). Without this second copy, switching Performance
    // focus to a state with a much shorter saved trim would make that short
    // span itself look like the whole loop, independently re-deriving a
    // bogus "original tempo" from it (combined with loopLengthBars) instead
    // of measuring it as a segment of the sample's real tempo — corrupting
    // getCalculatedOriginalBpm() and, since processBlock()'s repitchRatio is
    // shared by every trigger mode, the actual playback rate of whatever is
    // sounding at that moment too, Performance or not.
    std::atomic<int> tempoTrimStartSample { 0 };
    std::atomic<int> tempoTrimEndSample { 0 };

    // Audition (Step 25) — auditionActive is checked/cleared from the
    // audio thread (auto-stop) and set from the UI thread (button click);
    // auditionPosition is plain (not atomic) since it's only ever touched
    // under sampleLock — by processBlock()/renderAudition() on the audio
    // thread, and by setAuditionActive() on the UI thread.
    std::atomic<bool> auditionActive { false };
    double auditionPosition = 0.0;

    // Audition playhead (Step 28) — lock-free, written by renderAudition()
    // every block (audio thread), read by WaveformDisplay's 30fps timer
    // (UI thread), same pattern as currentlyPlayingSliceIndexForUI below.
    std::atomic<int> auditionPlaybackPositionForUI { -1 };

    // Manual BPM override (Step 23) — off by default, so behaviour is
    // unchanged (bars-derived tempo, same as always) until the user
    // explicitly enables it. 120 is just a sane inert starting value; it
    // has zero effect while disabled.
    std::atomic<bool> manualBpmOverrideEnabled { false };
    std::atomic<double> manualBpmOverrideValue { 120.0 };

    std::atomic<float> currentSensitivity { defaultSensitivity };
    std::atomic<float> fadeInMs { 5.0f };
    std::atomic<float> fadeOutMs { 15.0f };

    // Quantize detected transients to grid (Step 35) -- off by default,
    // same "preserve existing behaviour until explicitly opted into"
    // convention as every other toggle in this class. quantizeGridIndex
    // defaults to index 13 (4n / one quarter note), the same default
    // clockReferenceIndex already uses, for a consistent "quarter note"
    // starting point across every note-value-palette control.
    std::atomic<bool> quantizeTransientsEnabled { false };
    std::atomic<int> quantizeGridIndex { 13 };

    // Trim Snap mode (Performance mode's per-state trims only) -- off
    // (Transients) by default, same "preserve existing behaviour until
    // explicitly opted into" convention as quantizeTransientsEnabled above.
    // performanceTrimGridIndex defaults to index 13 (4n / one quarter
    // note), the same default every other note-value-palette control here
    // uses.
    std::atomic<TrimSnapMode> performanceTrimSnapMode { TrimSnapMode::transients };
    std::atomic<int> performanceTrimGridIndex { 13 };

    std::atomic<TriggerMode> triggerMode { TriggerMode::sliceLength };
    std::atomic<int> clockReferenceIndex { 13 }; // default: 4n / one quarter note (index in the expanded 20-value table)
    std::vector<float> subdivisionProbabilities; // size numNoteValueOptions, init to 1.0 each
    std::vector<float> playbackStyleProbabilities; // size numPlaybackStyleOptions, init to {1.0, 0.0, 0.0, 0.0, 0.0, 0.0} (Forward-only)
    std::vector<bool> randomizeParametersForStyle = std::vector<bool> ((size_t) numPlaybackStyleOptions, false); // see getRandomizeParametersForStyle()'s own doc comment
    std::atomic<TapeStopScope> tapeStopScope { TapeStopScope::wholeWindow };
    std::atomic<FilterSweepScope> filterSweepScope { FilterSweepScope::perTick };

    // Slice Length periodic reset (Step 34) -- index into {1, 2, 4, 8}
    // bars (see getResetBarsValue()). Defaults to index 2 (4 bars): a
    // reasonable middle ground that still resyncs regularly without
    // interrupting typical short phrases too aggressively. Unlike every
    // other toggle in this class, this one is intentionally NOT "off by
    // default to preserve existing behaviour" -- the whole feature is
    // mandatory, so Slice Length mode's playback genuinely changes (for
    // the better) the moment this exists, per the explicit decision that
    // this isn't optional.
    std::atomic<int> resetBarsIndex { 2 };

    // Sequenced Trigger Mode (Step 37) -- stepResolutionIndex indexes the
    // same note-value palette as clockReferenceIndex/quantizeGridIndex.
    // sequencerGrid is the pattern itself: flat-indexed as
    // row*getSequencerNumSteps()+column, resized (and reset to all-empty)
    // by resetSequencerGrid() whenever either dimension changes. Guarded
    // by sampleLock, same as slices/manualPoints -- read on the audio
    // thread every sample Sequenced mode is active, written from the UI
    // thread on every mouse-drawn cell. Each entry is -1 (empty) or a
    // PlaybackStyle index (0 to numPlaybackStyleOptions-1, Step 41) -- an
    // int, not a bool, since a cell now also remembers WHICH style it
    // should play.
    std::atomic<int> stepResolutionIndex { 7 }; // default: 16n (a sixteenth note)
    std::atomic<int> patternLengthBarsIndex { 0 }; // default: 1 bar
    std::vector<int> sequencerGrid;

    // Sequencer step parameter overrides (Step 45) -- sparse, keyed by the
    // same flat index (row*getSequencerNumSteps()+column) sequencerGrid
    // itself uses. A cell only appears here at all once it has at least
    // one override; absent means "use the global default" for every
    // parameter. Guarded by sampleLock, same lifecycle as sequencerGrid.
    std::map<int, std::map<juce::String, float>> sequencerCellParameterOverrides;

    // Step-extension (Pass 1) -- sparse, same flat-index key
    // (row*getSequencerNumSteps()+column) and lifecycle as the map above:
    // absent means "unset" (natural slice length). See
    // getSequencerCellExtendedLengthSteps()/setSequencerCellExtendedLengthSteps()
    // above for how it's read/written.
    std::map<int, int> sequencerCellExtendedLengthSteps;

    // Sequencer pattern bank (MIDI input, Pass 1) -- 128 slots, indexed 1:1
    // by MIDI note number, each either empty (default) or a full
    // SequencerPatternSnapshot. Guarded by sampleLock, same lifecycle as
    // sequencerGrid itself: read on the audio thread from
    // handleSequencerPatternRecallNoteOn()/completeMidiLearn() (called from
    // processBlock(), lock already held there), written from the UI thread
    // via armMidiLearnForPatternSave(). pendingSaveSnapshot holds whatever
    // was captured at the moment "Save to..." was clicked, until a note-on
    // arrives to claim a slot for it (or cancelMidiLearn() discards it).
    std::array<SequencerPatternSnapshot, 128> patternBank;
    SequencerPatternSnapshot pendingSaveSnapshot;
    std::atomic<bool> midiLearnArmed { false };
    std::atomic<int> activePatternBankSlot { -1 }; // -1 = no recall yet this session

    // Performance mode state bank -- same guard/lifecycle as patternBank
    // just above, just holding PerformanceStateSnapshot instead of
    // SequencerPatternSnapshot. performanceWorkingState is the one slot
    // with no counterpart in the Sequencer bank: since Performance mode has
    // no live grid of its own to capture wholesale (unlike sequencerGrid),
    // its style/params/loop/sync need somewhere to live WHILE being edited,
    // ahead of the next auto-save -- this is that somewhere. Whenever the
    // FOCUSED slot's own key is what's sounding, it's ALSO what
    // processBlock()'s performanceMode branch renders directly (deliberately
    // the same object, not a frozen copy -- see performancePlaybackIsFocused/
    // currentlyPlayingPerformanceSnapshot below for the OTHER-slot case), so
    // a parameter tweak made while the focused slot is still sounding (e.g.
    // Loop on) is heard on its very next pick with no explicit save required
    // -- the same "hear it as you shape it" contract every other
    // live-editable surface in this plugin already gives (waveform
    // probability drag, etc.). focusedPerformanceStateSlot tracks WHICH slot
    // currently has editing focus (set only by setFocusedPerformanceStateSlot(),
    // called from the on-screen keyboard's click handler) -- for the
    // keyboard's own focus highlight, and to decide, on each physical
    // note-on, whether that note IS the focused slot (live-audition) or some
    // OTHER slot (frozen snapshot playback) in handlePerformanceStateNoteOn().
    std::array<PerformanceStateSnapshot, 128> performanceStateBank;
    PerformanceStateSnapshot performanceWorkingState;
    std::atomic<int> focusedPerformanceStateSlot { -1 }; // -1 = nothing focused yet

    // Control mode -- base note/Gate setting are plain global atomics. No
    // keyswitch storage at all: getControlKeyswitchNote() computes each
    // style's note directly from controlBaseNote.
    std::atomic<int> controlBaseNote { 36 }; // C1, Ableton/Simpler numbering (MIDI 60 == C3)
    std::atomic<bool> controlGateModeActive { false }; // false = Trigger (default), true = Gate
    std::atomic<int> controlActiveStyle { 0 }; // PlaybackStyle index selected by the last keyswitch pressed

    // Pattern Switch Timing (Pass 2) -- see the public enum's own doc
    // comment above. patternSwitchIntervalIndex defaults to index 19 ("1n",
    // one bar) -- a coarser default than clockReferenceIndex's one-beat
    // default, since switching an entire pattern is a coarser action than
    // Clock mode's own per-slice picks. pendingPatternSwitchNote is -1
    // whenever no switch is pending; written from the audio thread only
    // (dispatchNoteOn and the per-sample boundary check both run inside
    // processBlock()), read from the UI thread for the "pending" indicator.
    std::atomic<PatternSwitchTiming> patternSwitchTiming { PatternSwitchTiming::immediate };
    std::atomic<int> patternSwitchIntervalIndex { 19 };
    std::atomic<int> pendingPatternSwitchNote { -1 };

    // Quantize Recall (Performance mode) -- see the public getters/setters'
    // own doc comment above for the overall design; same shape as Pattern
    // Switch Timing's Set Interval mode just above, just off by default
    // (preserving Performance mode's original immediate-recall behaviour)
    // rather than defaulting to a deferred mode the way Sequenced mode's
    // Pattern Switch Timing does. performanceQuantizeRecallIntervalIndex
    // defaults to index 13 (4n / one quarter note), the same default every
    // other note-value-palette control in this class uses (unlike
    // patternSwitchIntervalIndex's own coarser one-bar default -- recalling
    // a single performance state is a finer-grained action than switching
    // an entire pattern). pendingPerformanceRecallNote is -1 whenever no
    // recall is pending; written from the audio thread only (dispatchNoteOn
    // and the per-sample boundary check both run inside processBlock()),
    // read from the UI thread for the "pending" getter.
    std::atomic<bool> performanceQuantizeRecallEnabled { false };
    std::atomic<int> performanceQuantizeRecallIntervalIndex { 13 };
    std::atomic<int> pendingPerformanceRecallNote { -1 };

    // Style Palette's persistent "currently selected drawing style" (Step
    // 41) -- defaults to Forward (index 0).
    std::atomic<int> selectedDrawingStyle { 0 };

    // Stretch (Step 22) character parameters -- grain size/window shape
    // stay separate from Pitch Mode's own user-facing grain size/window
    // shape/pitch shift controls, none of which apply here. Grain size and
    // "speed" (a fixed per-pass character constant, independent of how
    // long the pick actually plays -- see its own doc comment above) were
    // fixed constants until Step 46 -- see setStretchGrainSizeMs()/
    // getStretchGrainSizeMs() and setStretchSpeedMultiplier()/
    // getStretchSpeedMultiplier() above, plus the per-step overrides these
    // feed via currentPickStretchGrainSizeMs/currentPickStretchSpeedMultiplier
    // below. Small grains + a hard-edged window (still fixed, not exposed)
    // make the seams audible.
    std::atomic<float> stretchGrainSizeMsValue { defaultStretchGrainSizeMs };
    std::atomic<float> stretchSpeedMultiplierValue { defaultStretchSpeedMultiplier };

    // Filter Down/Filter Up (Step 29/30) character parameters —
    // filterSweepStartHz/filterSweepEndHz remain deliberately fixed, no
    // exposed controls, same "defer the knob until proven necessary"
    // pattern as Stretch's grain size above. filterSweepStartHz/
    // filterSweepEndHz are Filter Down's open->closed endpoints (the
    // classic breakbeat/DnB "filter close"); Filter Up just swaps which
    // endpoint it starts/ends at (see processBlock()) -- no separate
    // constants needed. Resonance is no longer fixed -- see
    // filterSweepResonanceValue below (Step 45). One shared filter
    // instance is fine — Playback Style is a single mutually-exclusive
    // pick per pick, so it's never touched by more than one pick's
    // processing at a time.
    static constexpr float filterSweepStartHz = 9000.0f;
    static constexpr float filterSweepEndHz = 250.0f;
    juce::dsp::StateVariableTPTFilter<float> filterSweepFilter;

    // Bitcrush state (Step 48/49) -- sample-and-hold downsampler needs to
    // remember the last "grabbed" value per channel and how many samples
    // are left before the next grab, both reset alongside filterSweepFilter
    // in the shared pickJustStarted block so a new pick never inherits the
    // previous pick's hold phase or held value. The hold LENGTH and bit
    // DEPTH themselves are no longer fixed constants (Step 49 made them
    // per-step adjustable, each with its own Static/Sweep In/Sweep Out
    // mode -- see currentPickBitcrushRateValue/Mode and
    // currentPickBitcrushBitDepthValue/Mode above), just this held-sample
    // bookkeeping is unaffected by that -- it doesn't care WHY the hold
    // length changed from one grab to the next, only that it did.
    float bitcrushHeldSample[GranularStretcher::maxChannels] = {};
    int bitcrushHoldCounter = 0;

    // Flanger delay line -- a short circular buffer per channel (with its
    // own adjustable Feedback amount feeding the delayed signal back into
    // the line, see applyFlanger() in processBlock()), sized in
    // prepareToPlay() to comfortably hold
    // flangerDelayTimeExtremeMs of audio at the real host sample rate
    // (sample rate isn't known at construction, same reason
    // filterSweepFilter itself is only prepare()'d there rather than at
    // construction). Reset (cleared, write index rewound to 0) alongside
    // bitcrushHeldSample/bitcrushHoldCounter above in the shared
    // pickJustStarted block, so a fresh pick's comb character starts from
    // silence rather than inheriting whatever the previous pick (of any
    // style) left sitting in the line -- same "self-contained within one
    // pick's lifetime" contract every other style's per-pick state already
    // has.
    juce::AudioBuffer<float> flangerDelayBuffer;
    int flangerDelayWriteIndex = 0;

    // Filter Sweep resonance (Step 45) -- see setFilterSweepResonance()/
    // getFilterSweepResonance() above. ~2.0 approximates the requested
    // Q~2-3 range in this filter class's own "resonance" parameter (per
    // juce::dsp::StateVariableTPTFilter's docs, standard 12dB/octave --
    // i.e. no added resonance -- is 1/sqrt(2); higher values add
    // character without self-oscillating at this level).
    std::atomic<float> filterSweepResonanceValue { defaultFilterSweepResonance };

    // Filter Sweep filter type (Step 46) -- see setFilterSweepFilterType()/
    // getFilterSweepFilterType() above. 0 = lowpass, matching the filter's
    // original hardcoded setType() call.
    std::atomic<int> filterSweepFilterTypeValue { 0 };

    // Curve shape (Step 46) -- see setCurveShape()/getCurveShape() above.
    // 0 = linear, matching Tape Stop/Ping-Pong's existing behaviour.
    std::atomic<int> curveShapeValue { 0 };

    // Bitcrush/Flanger/Scratch global defaults -- see set*Global()/
    // get*Global() above. Defaults match the original fixed constants
    // (bitcrushRateReductionDefault etc. in PluginProcessor.cpp) exactly.
    std::atomic<float> bitcrushRateReductionGlobalValue { 12.0f };
    std::atomic<int> bitcrushRateReductionModeGlobalValue { 0 };
    std::atomic<float> bitcrushBitDepthGlobalValue { 5.0f };
    std::atomic<int> bitcrushBitDepthModeGlobalValue { 0 };
    std::atomic<int> scratchRateGlobalValue { 7 }; // 16n, matching scratchDefaultRateIndex
    std::atomic<int> scratchForwardCurveGlobalValue { 0 }; // Linear
    std::atomic<int> scratchBackwardCurveGlobalValue { 0 }; // Linear
    std::atomic<float> flangerDelayTimeGlobalValue { 2.0f };
    std::atomic<int> flangerDelayTimeModeGlobalValue { 0 };
    std::atomic<float> flangerMixGlobalValue { 0.5f };
    std::atomic<int> flangerMixModeGlobalValue { 0 };
    std::atomic<float> flangerFeedbackGlobalValue { 0.3f };
    std::atomic<int> flangerFeedbackModeGlobalValue { 0 };

    // This pick's own resonance/filter type/curve shape (Step 45/46),
    // captured once at pick-start by every trigger mode (Slice Length/
    // Clock always capture the global value; Sequenced mode captures its
    // step's own override if present, else the global value) and applied
    // once in the shared pickJustStarted block (resonance/filter type) or
    // consulted directly during rendering (curve shape) below --
    // audio-thread-only state, same pattern as currentPickBeatQuantized/
    // currentPickTapeStopDurationHostSamples.
    float currentPickFilterSweepResonance = defaultFilterSweepResonance;
    int currentPickFilterSweepType = 0;
    int currentPickCurveShape = 0;

    // This pick's own Stretch grain size/speed (Step 46), same capture
    // pattern as currentPickFilterSweepResonance just above -- Slice
    // Length/Clock always capture the global value; Sequenced mode
    // captures its step's own override if present. Harmless (unused) for
    // every style but Stretch, same as currentPickTapeStopDurationHostSamples
    // is for everything but Tape Stop.
    float currentPickStretchGrainSizeMs = defaultStretchGrainSizeMs;
    float currentPickStretchSpeedMultiplier = defaultStretchSpeedMultiplier;

    // This pick's own Bitcrush Sample Rate Reduction/Bit Depth VALUE and
    // MODE (Step 49), same capture pattern as currentPickFilterSweepResonance
    // above -- Slice Length/Clock always capture the fixed default value
    // with Static mode (no global dial for either, same as Subdivide);
    // Sequenced mode captures its step's own override if present. Harmless
    // (unused) for every style but Bitcrush. Mode is Static (0) rather
    // than an enum class -- consulted only via plain int comparison in
    // processBlock's sweep-interpolation lambda, same convention
    // currentPickFilterSweepType/currentPickCurveShape already use for
    // their own small fixed option sets.
    float currentPickBitcrushRateValue = 0.0f;
    int currentPickBitcrushRateMode = 0;
    float currentPickBitcrushBitDepthValue = 0.0f;
    int currentPickBitcrushBitDepthMode = 0;

    // This pick's own Flanger Delay Time/Mix/Feedback VALUE and MODE,
    // identical capture pattern to currentPickBitcrushRateValue/Mode
    // above -- Slice Length/Clock always capture the fixed default value
    // with Static mode (no global dial for any of the three); Sequenced
    // mode captures its step's own override if present. Harmless (unused)
    // for every style but Flanger.
    float currentPickFlangerDelayValue = 0.0f;
    int currentPickFlangerDelayMode = 0;
    float currentPickFlangerMixValue = 0.0f;
    int currentPickFlangerMixMode = 0;
    float currentPickFlangerFeedbackValue = 0.0f;
    int currentPickFlangerFeedbackMode = 0;

    // This pick's own Volume ramp VALUE and MODE (style-independent) --
    // captured ONLY by Sequenced mode's step-trigger block, unlike every
    // other currentPick* pair above, since Volume has no global dial and
    // is deliberately not offered in Slice Length/Clock mode (see
    // isSequencerCellParameterSwept()'s own doc comment). Defaults here
    // (1.0/Static, i.e. full volume, unchanged) are the "no ramp" no-op,
    // matching what an absent override resolves to anyway -- but
    // processBlock() still gates its use behind `sequencedMode` itself
    // rather than relying on these defaults alone, so a value captured
    // during a previous Sequenced-mode step can never bleed into Slice
    // Length/Clock mode playback after switching modes.
    float currentPickVolumeValue = 1.0f;
    int currentPickVolumeMode = 0;

    // This pick's own Delay/Reverb bus Send Amount (Pass 2) -- style-
    // independent, opt-in per step, same "captured ONLY by Sequenced
    // mode's step-trigger block" pattern as currentPickVolumeValue just
    // above (there's no global Send Amount dial anywhere -- see
    // EffectsBusPanel -- so, unlike Filter Sweep/Bitcrush/Flanger, no other
    // trigger mode has an equivalent global value to fall back to for
    // these; processSendBuses() reads a fixed 50% send instead whenever
    // Sequenced mode isn't the active trigger mode, matching this
    // feature's Pass 1 behaviour there unchanged). Defaults to 0.0f (no
    // send) -- the "no override yet" fallback getSequencerCellParameterOverride()
    // resolves to, matching this feature's own "opt-in, not automatic"
    // spec.
    float currentPickDelaySendAmount = 0.0f;
    float currentPickReverbSendAmount = 0.0f;

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

    // Slice Length mode's periodic reset (Step 34, audio thread only) --
    // a lightweight, independent version of the window-boundary tracking
    // just above: resetWindowInitialized false forces a fresh window +
    // fresh pick on next block (transport start, or entering Slice Length
    // mode), same "always start aligned" behaviour clockModeInitialized
    // already gives Clock mode. resetWindowEndPpq is checked every SAMPLE
    // in the Slice Length branch below, not once per block -- the exact
    // bug Step 6 introduced and fixed was computing a boundary once per
    // block from the block's start position, silently missing boundaries
    // that fell mid-block; this reuses Clock mode's own per-sample
    // newWindow check directly rather than re-deriving that logic.
    bool resetWindowInitialized = false;
    double resetWindowEndPpq = 0.0;

    // Sequenced Trigger Mode (Step 37, audio thread only) --
    // sequencedModeInitialized false forces the very first per-sample
    // check on next block to treat the current step as new (transport
    // start, or entering Sequenced mode), same "always start aligned"
    // guarantee clockModeInitialized/resetWindowInitialized already give
    // their own modes. sequencedLastStepIndex is what that per-sample
    // check compares against to detect a genuine step-boundary crossing
    // -- -1 is never a valid step index, so it always counts as "new" the
    // first time.
    bool sequencedModeInitialized = false;
    int sequencedLastStepIndex = -1;

    // Performance mode (audio thread only) -- performanceModeInitialized
    // false forces the very first per-sample check on next block to start
    // SILENT (hasCurrentPick = false), NOT with an immediately-forced pick
    // the way clockModeInitialized/resetWindowInitialized/sequencedModeInitialized
    // all do for their own modes -- Performance mode has nothing to play
    // until a note-on triggers a pick. performanceRecallPending is the
    // same-call handoff from handlePerformanceStateNoteOn() (set true
    // there, consumed and cleared on the very next per-sample check below)
    // that sequencedModeInitialized already models for pattern recall.
    bool performanceModeInitialized = false;
    bool performanceRecallPending = false;

    // Which source governs the pick that performanceRecallPending is about
    // to start (or that's already sounding) -- true: performanceWorkingState,
    // the focused slot's own live/in-progress edits (played via the shared
    // trim atomics every other mode also edits through); false:
    // currentlyPlayingPerformanceSnapshot, a frozen copy of some OTHER
    // slot's saved state (including its own saved trim), copied in at the
    // note-on that started this pick so auditioning it can never disturb
    // performanceWorkingState/focus. Both set together, only from
    // handlePerformanceStateNoteOn() -- audio-thread-only, same convention
    // as every other currentPick*/performance* field in this section.
    bool performancePlaybackIsFocused = false;
    PerformanceStateSnapshot currentlyPlayingPerformanceSnapshot;

    // Control mode (audio thread only) -- controlModeInitialized/
    // controlNoteOnPending mirror performanceModeInitialized/
    // performanceRecallPending exactly: starts and stays silent until a
    // slice note-on arrives, consumed on the very next per-sample check.
    // The controlPending* fields are the same-call handoff from
    // handleControlNoteOn() to that check (slice index, style, velocity gain,
    // and the actual note number, needed so a later Gate release can confirm
    // it's releasing the note that's actually still sounding, not a stale
    // one already superseded by monophonic retrigger).
    bool controlModeInitialized = false;
    bool controlNoteOnPending = false;
    int controlPendingSliceIndex = -1;
    int controlPendingStyle = 0;
    double controlPendingVelocityGain = 1.0;
    int controlPendingNoteNumber = -1;
    int controlCurrentlySoundingNote = -1;
    double currentControlVelocityGain = 1.0; // captured from controlPendingVelocityGain at pick-start, held for that pick's whole duration

    // Gate release (audio thread only) -- controlGateReleaseActive arms a
    // fadeOutMs-long gain ramp (computed in the shared per-sample gain
    // stage, alongside Volume's own ramp) independent of whichever style's
    // own duration/completion math is in play, so it works identically
    // across all 9 PlaybackStyles without special-casing any of them.
    // controlGateReleaseElapsedSamples counts real time since the release
    // note-off arrived; once it reaches fadeOutSamplesRequested the ramp
    // has reached silence and processBlock() forces hasCurrentPick false.
    bool controlGateReleaseActive = false;
    double controlGateReleaseElapsedSamples = 0.0;

    // Set Interval pattern-switch scheduling (Pass 2, audio thread only) --
    // patternSwitchIntervalBoundaryArmed false means "next occurrence not
    // computed yet," forcing the per-sample check in processBlock() to snap
    // patternSwitchIntervalBoundaryPpq to the next grid point fresh from
    // wherever ppq currently is, the moment a switch gets (re-)armed --
    // same "arm now, resolve against the very next per-sample check" shape
    // clockModeInitialized/resetWindowInitialized use for their own first
    // boundary. Re-armed (set back to false) every time
    // pendingPatternSwitchNote changes, including on replacement by a newer
    // note-on -- always tracks "next occurrence from NOW," not from
    // whenever the original note-on arrived.
    bool patternSwitchIntervalBoundaryArmed = false;
    double patternSwitchIntervalBoundaryPpq = 0.0;

    // Quantize Recall scheduling (Performance mode, audio thread only) --
    // exactly the same "arm now, resolve against the very next per-sample
    // check" shape as patternSwitchIntervalBoundaryArmed/Ppq just above,
    // just for pendingPerformanceRecallNote instead of
    // pendingPatternSwitchNote. Re-armed (set back to false) every time
    // pendingPerformanceRecallNote changes, including on replacement by a
    // newer note-on -- always tracks "next occurrence from NOW."
    bool performanceQuantizeRecallBoundaryArmed = false;
    double performanceQuantizeRecallBoundaryPpq = 0.0;

    // Subdivide (Step 47, audio thread only) -- per-step retrigger rate,
    // Sequenced mode only. Captured once at a step's own pick-start (see
    // the sequencedMode branch in processBlock) from that cell's
    // "Subdivide" override; sequencedSubdivisionActive false (the
    // default -- Off/no override) means nothing here runs and playback
    // is identical to before this feature existed.
    // sequencedSubdivisionRow is which slice to restart on each
    // retrigger -- cached rather than re-read from currentStepIndex,
    // since currentStepIndex keeps advancing for as long as this note
    // sustains across later (inactive) step columns, while this note's
    // own row doesn't change.
    // sequencedNextSubdivisionOffsetHostSamples/
    // sequencedSubdivisionTickLengthHostSamples are host-sample-domain
    // scheduling state measured against samplesSinceWindowStart/
    // currentWindowLengthHostSamples just below -- reused directly
    // rather than a separate ppq-based scheduler, since those already
    // track "how far into this step's own window are we" (and, for
    // Filter Down/Up, already drive the Whole Window sweep -- see
    // useWholeWindow in processBlock, generalized to also cover a
    // subdivided Sequenced step's window).
    bool sequencedSubdivisionActive = false;
    int sequencedSubdivisionRow = -1;
    double sequencedNextSubdivisionOffsetHostSamples = 0.0;
    double sequencedSubdivisionTickLengthHostSamples = 0.0;

    // Lock-free copy of the currently active step column (Step 37),
    // written by the audio thread every time a new step boundary is
    // reached, read by the UI thread for the sequencer grid's playhead
    // indicator -- same pattern as currentlyPlayingSliceIndexForUI below.
    std::atomic<int> currentlyPlayingStepIndexForUI { -1 };

    // Filter Sweep's Whole Window scope (Step 30) — how far into the
    // CURRENT WINDOW we are, in host samples, as opposed to samplesSince-
    // PickStart's per-pick tracking just below. Reset only on a genuine
    // new-window event (never on an ordinary per-tick retrigger, unlike
    // samplesSincePickStart), so it stays continuous across every tick
    // inside one window. currentWindowLengthHostSamples is set alongside
    // it, at the same new-window event, from whatever the clock reference
    // note value resolves to in host samples at that moment — both were
    // Clock-mode-only through Step 46 and are meaningless in Slice Length
    // mode (which has no concept of a "window").
    // Step 47 (Subdivide) reuses this exact same pair for Sequenced mode
    // too: "window" there means one currently-playing step's own
    // (already monophony-clamped) total duration, reset at that step's
    // pick-start instead of a recurring Clock window -- this is what
    // lets a subdivided Filter Down/Up step's sweep glide continuously
    // across the whole step while individual retriggers happen
    // underneath, and also doubles as the Subdivide retrigger scheduler
    // itself (see sequencedNextSubdivisionOffsetHostSamples above).
    double samplesSinceWindowStart = 0.0;
    double currentWindowLengthHostSamples = 0.0;

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

    // Scratch (v1): this pick's own bounce-cycle length in host samples,
    // captured once at pick-start by every trigger mode via
    // computeScratchCycleLengthHostSamples() -- see that function's own
    // doc comment. Meaningless/unused for every other style.
    double currentPickScratchCycleLengthHostSamples = 0.0;

    // Scratch (v2): this pick's own Forward/Backward Curve choice --
    // captured once at pick-start, same per-step-override-else-fixed-
    // default pattern as every other Scratch/Bitcrush parameter (Slice
    // Length/Clock modes always Linear/Linear; Sequenced mode reads this
    // step's own override if it has one). Fed into both render paths'
    // foldPosition()/renderAndAdvance() calls in the shared per-sample
    // section below. Meaningless/unused for every other style.
    EasingCurve currentPickScratchForwardCurve = EasingCurve::linear;
    EasingCurve currentPickScratchBackwardCurve = EasingCurve::linear;

#if JUCE_DEBUG
    // TEMPORARY DEBUG (Tape Stop position-exhaustion verification) --
    // remove once step-extension Tape Stop testing is done.
    //
    // Edge-detection state for processBlock()'s Tape Stop render path --
    // plain (non-atomic) since it's only ever touched from the audio
    // thread itself, same as every other processBlock()-only member here.
    // Reset at every pick-start so each pick's own transitions are
    // detected fresh rather than carrying over stale state from whatever
    // played before it.
    bool debugTapeStopPrevWithinSchedule = false;
    bool debugTapeStopPrevExhausted = false;

    // TEMPORARY DEBUG (gain-ramp-vs-render-gate verification) -- same
    // reasoning/lifecycle as the two above. Edge-detects the sample the
    // Tape Stop gain ramp (tapeStopRateMultiplier) first drops below
    // debugTapeStopGainNearZeroThreshold, so its samplesSincePickStart can
    // be compared directly against currentPickTapeStopDurationHostSamples
    // -- confirms (or refutes) whether the ramp's own reach-zero point
    // actually lines up with the pick's full (possibly extended) duration,
    // as opposed to some shorter value.
    bool debugTapeStopPrevGainNearZero = false;
    static constexpr double debugTapeStopGainNearZeroThreshold = 0.02;

    // Lock-free "mailbox" -- the audio thread only ever does cheap atomic
    // stores into these (real-time-safe), never calls DBG()/console I/O
    // itself. drainDebugTapeStopEvents() (called from a UI-thread timer --
    // see SequencerGrid's) polls the *Pending flags and does the actual
    // printing off the audio thread entirely. Calling DBG() directly from
    // inside processBlock() -- which this replaces -- did string
    // formatting and a blocking console-I/O syscall while sampleLock was
    // held, which could stall the audio callback long enough that every
    // UI-thread call needing that same lock (SequencerGrid's own 30fps
    // poll among them) blocked too, freezing the whole app.
    std::atomic<bool> debugTapeStopPickStartEventPending { false };
    std::atomic<int> debugTapeStopPickStartRow { -1 };
    std::atomic<int> debugTapeStopPickStartStep { -1 };
    std::atomic<int> debugTapeStopPickStartDeclaredLengthSteps { 0 };
    std::atomic<double> debugTapeStopPickStartDeclaredLengthHostSamples { 0.0 };
    std::atomic<double> debugTapeStopPickStartSamplesUntilNextActiveStep { 0.0 };
    std::atomic<double> debugTapeStopPickStartDurationHostSamples { 0.0 };

    std::atomic<bool> debugTapeStopFreezeEventPending { false };
    std::atomic<double> debugTapeStopFreezeSamplesSincePickStart { 0.0 };
    std::atomic<double> debugTapeStopFreezeDurationHostSamples { 0.0 };
    std::atomic<double> debugTapeStopFreezePosition { 0.0 };
    std::atomic<int> debugTapeStopFreezeSchedulingEndSample { 0 };

    std::atomic<bool> debugTapeStopStopEventPending { false };
    std::atomic<double> debugTapeStopStopSamplesSincePickStart { 0.0 };
    std::atomic<double> debugTapeStopStopDurationHostSamples { 0.0 };
    std::atomic<bool> debugTapeStopStopEverFroze { false };

    // TEMPORARY DEBUG (gain-ramp-vs-render-gate verification) -- see
    // debugTapeStopPrevGainNearZero's own doc comment above for why this
    // exists.
    std::atomic<bool> debugTapeStopGainNearZeroEventPending { false };
    std::atomic<double> debugTapeStopGainNearZeroSamplesSincePickStart { 0.0 };
    std::atomic<double> debugTapeStopGainNearZeroDurationHostSamples { 0.0 };
    std::atomic<double> debugTapeStopGainNearZeroGainValue { 0.0 };
    std::atomic<double> debugTapeStopGainNearZeroRateMultiplier { 0.0 };
    std::atomic<bool> debugTapeStopGainNearZeroWasExhausted { false };
    std::atomic<bool> debugTapeStopGainNearZeroWasGranular { false };

    // TEMPORARY DEBUG (Stretch step-extension verification) -- remove
    // together with the Tape Stop debug members above once this is done.
    // Originally added to confirm Stretch's target length had no
    // relationship to the step's own declared length
    // (getSequencerCellDeclaredLengthSteps(), the mechanism Tape Stop
    // already used) -- it didn't, and duration is now authoritative from
    // that same declared length (see processBlock()'s stretchActive
    // branches). passLengthHostSamples (speedMultiplier * natural, the
    // fixed-character length of ONE stretched pass) is logged alongside
    // finalLengthHostSamples so it's easy to tell, per pick, whether the
    // declared length actually required the new loop-to-repeat behaviour
    // (finalLength > passLength) or was covered by a single pass.
    // Same lock-free mailbox/drain pattern as the Tape Stop members
    // above -- see their own doc comment for why.
    std::atomic<bool> debugStretchPickStartEventPending { false };
    std::atomic<int> debugStretchPickStartRow { -1 };
    std::atomic<int> debugStretchPickStartStep { -1 };
    std::atomic<int> debugStretchPickStartDeclaredLengthSteps { 0 };
    std::atomic<double> debugStretchPickStartDeclaredLengthHostSamples { 0.0 };
    std::atomic<double> debugStretchPickStartNaturalLengthHostSamples { 0.0 };
    std::atomic<double> debugStretchPickStartSpeedMultiplier { 0.0 };
    std::atomic<double> debugStretchPickStartPassLengthHostSamples { 0.0 };
    std::atomic<double> debugStretchPickStartSamplesUntilNextActiveStep { 0.0 };
    std::atomic<double> debugStretchPickStartFinalLengthHostSamples { 0.0 };

    // TEMPORARY DEBUG (Forward/Ping-Pong/Filter Down/Up step-extension
    // verification) -- remove together with the debug members above once
    // this is done. Same shape/mailbox pattern as the Tape Stop/Stretch
    // pick-start logs -- styleName distinguishes which of the three fired
    // (they share one branch in processBlock()). naturalLengthHostSamples
    // logged alongside finalLengthHostSamples so it's easy to tell, per
    // pick, whether the declared length actually required looping past
    // one natural unit (finalLength > natural) or was covered by it.
    std::atomic<bool> debugLoopStylePickStartEventPending { false };
    std::atomic<int> debugLoopStylePickStartRow { -1 };
    std::atomic<int> debugLoopStylePickStartStep { -1 };
    // PlaybackStyle ordinal (indexToPlaybackStyle()'s own numbering), NOT a
    // juce::String -- constructing/assigning a String from a literal
    // allocates, which isn't real-time-safe on the audio thread (the exact
    // class of bug the DBG()-on-the-audio-thread freeze earlier this
    // session was). drainDebugStretchEvents() maps this to a name for
    // printing, on the UI thread where allocating is fine.
    std::atomic<int> debugLoopStylePickStartStyleIndex { -1 };
    std::atomic<int> debugLoopStylePickStartDeclaredLengthSteps { 0 };
    std::atomic<double> debugLoopStylePickStartDeclaredLengthHostSamples { 0.0 };
    std::atomic<double> debugLoopStylePickStartNaturalLengthHostSamples { 0.0 };
    std::atomic<double> debugLoopStylePickStartSamplesUntilNextActiveStep { 0.0 };
    std::atomic<double> debugLoopStylePickStartFinalLengthHostSamples { 0.0 };
#endif

#if JUCE_DEBUG
    // TEMPORARY DEBUG (Performance mode click-to-focus freeze investigation)
    // -- remove once this is done. Same "lock-free mailbox, no I/O on the
    // audio thread" discipline as the Tape Stop/Stretch debug members
    // above (see their own doc comment) -- the audio thread only ever does
    // atomic stores here. A dedicated juce::HighResolutionTimer thread
    // (deliberately NOT the message thread, which is the one under
    // suspicion of being the one that's stuck) polls these once a second
    // and prints directly, so the log keeps updating even if the message
    // thread is wedged inside setFocusedPerformanceStateSlot().
    std::atomic<juce::int64> debugAudioProcessBlockEntries { 0 }; // incremented at the very top of processBlock(), before sampleLock is even attempted
    std::atomic<juce::int64> debugAudioLockAcquiredCount { 0 };   // incremented immediately after processBlock() acquires sampleLock
    std::atomic<bool> debugFocusChangeInProgress { false };       // true for the duration of setFocusedPerformanceStateSlot() -- set before attempting sampleLock, cleared at the very end
    std::atomic<juce::int64> debugFocusChangeStartMs { 0 };
    std::atomic<int> debugFocusChangeNoteNumber { -1 };

    struct FreezeWatchdog : public juce::HighResolutionTimer
    {
        explicit FreezeWatchdog (SlicerAudioProcessor& ownerToUse) : owner (ownerToUse) {}
        void hiResTimerCallback() override;
        SlicerAudioProcessor& owner;
    };

    FreezeWatchdog freezeWatchdog { *this };
#endif

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

    // Performance mode's Sync toggle (Pass 1, audio thread only) -- captured
    // once at pick-start, same shape as currentPickBeatQuantized just above:
    // explicitly false for every OTHER mode's picks (Clock/Sequenced/Slice
    // Length), and set to `! performanceWorkingState.sync` for a Performance
    // pick. When true, processBlock() substitutes a native (sampleSampleRate/
    // hostSampleRate) rate for playbackRate at every downstream use, instead
    // of whichever global Pitch Mode (Repitch/Time-Stretch) is currently
    // selected -- captured once here, rather than checked inline at each of
    // those several downstream sites, so a pick's rate can never read as one
    // thing at pick-start and another mid-render.
    bool currentPickNativeRateActive = false;

    static constexpr float defaultSensitivity = 0.5f; // sensible starting sensitivity

    // Fixed-ms holdoff floor — ONLY used as computeMinimumHoldoffMs()'s
    // fallback when there's no usable tempo yet (no sample loaded). Every
    // actual detection call site uses computeMinimumHoldoffMs() instead of
    // this directly; see that method's doc comment for why (tempo-relative
    // holdoff replaced this as the real minimum-gap floor).
    static constexpr float defaultHoldoffMs = 30.0f;

    //=== Delay/Reverb send buses (Pass 1) ===
    // Persistent effect state that exists independently of any pick's
    // lifecycle -- unlike every other effect in this engine (Filter Sweep/
    // Bitcrush/Flanger/Scratch), which is per-pick state reset at each new
    // pick's start, these run continuously across pick boundaries so a
    // ringing tail keeps decaying after the triggering pick has fully
    // stopped. See processSendBuses() in PluginProcessor.cpp.
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayBusLine;
    juce::dsp::Reverb reverbBusDSP;

    // Per-block scratch accumulation buffers -- each bus's own scaled copy
    // of this block's dry output, processed in place into that bus's wet
    // output. Sized in prepareToPlay() to samplesPerBlock; processSendBuses()
    // itself only ever calls setSize() with avoidReallocating == true, so
    // it never allocates on the audio thread as long as the host never
    // exceeds the block size it originally negotiated -- same contract
    // flangerDelayBuffer above already relies on.
    juce::AudioBuffer<float> delayBusBuffer;
    juce::AudioBuffer<float> reverbBusBuffer;

    // Delay bus parameters -- see setDelayBusTimeMs()/getDelayBusTimeMs()
    // etc. above. Defaults chosen to be audible immediately (Pass 1 proof
    // of concept), not tuned for any particular musical use.
    std::atomic<float> delayBusTimeMsValue { 350.0f };
    std::atomic<float> delayBusFeedbackValue { 0.35f };
    std::atomic<float> delayBusReturnLevelValue { 0.6f };

    // Reverb bus parameters -- see setReverbBusSize()/getReverbBusSize()
    // etc. above.
    std::atomic<float> reverbBusSizeValue { 0.5f };
    std::atomic<float> reverbBusDecayValue { 0.5f };
    std::atomic<float> reverbBusReturnLevelValue { 0.55f };

    // Character ramp TARGETS (Pass 2) -- what processSendBuses() eases the
    // four values just above toward, every block, rather than snapping to
    // instantly. Written from two places: the panel setters just above
    // (setDelayBusTimeMs() etc.), which re-arm both the live value AND its
    // target to the same number so a manual dial turn is never fought by a
    // stale pending ramp; and a firing Sequenced-mode step's own Delay Bus
    // Time/Delay Bus Feedback/Reverb Bus Size/Reverb Bus Decay override
    // (processBlock()'s sequencedMode branch), which moves ONLY the target,
    // audio-thread-side -- both writers land on the audio thread or the
    // message thread touching a single atomic float each, the same
    // lock-free cross-thread contract every other *GlobalValue atomic in
    // this class already relies on. Defaults match the live values' own
    // defaults just above, so the ramp starts as a no-op.
    std::atomic<float> delayBusTimeMsRampTargetValue { 350.0f };
    std::atomic<float> delayBusFeedbackRampTargetValue { 0.35f };
    std::atomic<float> reverbBusSizeRampTargetValue { 0.5f };
    std::atomic<float> reverbBusDecayRampTargetValue { 0.5f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessor)
};
